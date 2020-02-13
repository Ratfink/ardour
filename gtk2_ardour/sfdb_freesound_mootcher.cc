/* sfdb_freesound_mootcher.cpp **********************************************************************

	Adapted for Ardour by Ben Loftis, March 2008
	Updated to new Freesound API by Colin Fletcher, November 2011
	Updated to Freesound API v2 by Colin Fletcher, February 2016

	Mootcher 23-8-2005

	Mootcher Online Access to thefreesoundproject website
	http://freesound.iua.upf.edu/

	GPL 2005 Jorn Lemon
	mail for questions/remarks: mootcher@twistedlemon.nl
	or go to the freesound website forum

	-----------------------------------------------------------------

	Includes:
		curl.h    (version 7.14.0)
	Librarys:
		libcurl.lib

	-----------------------------------------------------------------
	Licence GPL:

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.


*************************************************************************************/

// #define OAUTH_BUILTIN_HACK 1
#include "sfdb_freesound_mootcher.h"

#include "pbd/xml++.h"
#include "pbd/error.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <iostream>

#include <glib.h>
#include "pbd/gstdio_compat.h"

#include "pbd/i18n.h"

#include "ardour/audio_library.h"
#include "ardour/debug.h"
#include "ardour/filesystem_paths.h"
#include "ardour/rc_configuration.h"
#include "pbd/pthread_utils.h"
#include "pbd/openuri.h"

#include "ardour_dialog.h"
#include "gui_thread.h"
#include "widgets/prompter.h"

using namespace PBD;

static const std::string base_url = "https://www.freesound.org/apiv2";

// Ardour 6
static const std::string default_token = "XCUUJnJlv5OggPrsHqRVgKc5QVukz4hyFXh9PEeE";
static const std::string client_id = "NvmN6EzlXSbp55ihN8jN";

static const std::string fields = "id,name,duration,filesize,samplerate,license,download,previews";

//------------------------------------------------------------------------
Mootcher::Mootcher(const std::string &the_token)
	: curl(curl_easy_init())
{
	DEBUG_TRACE(PBD::DEBUG::Freesound, "Created new Mootcher, oauth_token =\"" + the_token + "\"\n");
	custom_headers = NULL;
	if  (the_token != "") {
		oauth_token = the_token;
		std::string auth_header = "Authorization: Bearer " + oauth_token;
		DEBUG_TRACE(PBD::DEBUG::Freesound, "auth_header = " + auth_header + "\n");
		custom_headers = curl_slist_append (custom_headers, auth_header.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, custom_headers);
	} else {
		oauth_token = "";
	}
	cancel_download_btn.set_label (_("Cancel"));
	progress_hbox.pack_start (progress_bar, true, true);
	progress_hbox.pack_end (cancel_download_btn, false, false);
	progress_bar.show();
	cancel_download_btn.show();
	cancel_download_btn.signal_clicked().connect(sigc::mem_fun (*this, &Mootcher::cancelDownload));
};
//------------------------------------------------------------------------
Mootcher:: ~Mootcher()
{
	curl_easy_cleanup(curl);
	if (custom_headers) {
		curl_slist_free_all (custom_headers);
	}
	DEBUG_TRACE(PBD::DEBUG::Freesound, "Destroyed Mootcher\n");
}

//------------------------------------------------------------------------

void Mootcher::ensureWorkingDir ()
{
	std::string p = ARDOUR::Config->get_freesound_download_dir();

	DEBUG_TRACE(PBD::DEBUG::Freesound, "ensureWorkingDir() - " + p + "\n");
	if (!Glib::file_test (p, Glib::FILE_TEST_IS_DIR)) {
		if (g_mkdir_with_parents (p.c_str(), 0775) != 0) {
			PBD::error << "Unable to create Mootcher working dir" << endmsg;
		}
	}
	basePath = p;
#ifdef PLATFORM_WINDOWS
	std::string replace = "/";
	size_t pos = basePath.find("\\");
	while( pos != std::string::npos ){
		basePath.replace(pos, 1, replace);
		pos = basePath.find("\\");
	}
#endif
}


//------------------------------------------------------------------------
size_t Mootcher::WriteMemoryCallback(void *ptr, size_t size, size_t nmemb, void *data)
{
	int realsize = (int)(size * nmemb);
	struct SfdbMemoryStruct *mem = (struct SfdbMemoryStruct *)data;

	mem->memory = (char *)realloc(mem->memory, mem->size + realsize + 1);

	if (mem->memory) {
		memcpy(&(mem->memory[mem->size]), ptr, realsize);
		mem->size += realsize;
		mem->memory[mem->size] = 0;
	}
	return realsize;
}


//------------------------------------------------------------------------

std::string Mootcher::sortMethodString(enum sortMethod sort)
{
// given a sort type, returns the string value to be passed to the API to
// sort the results in the requested way.

	switch (sort) {
		case sort_duration_descending:  return "duration_desc";
		case sort_duration_ascending:   return "duration_asc";
		case sort_created_descending:   return "created_desc";
		case sort_created_ascending:    return "created_asc";
		case sort_downloads_descending: return "downloads_desc";
		case sort_downloads_ascending:  return "downloads_asc";
		case sort_rating_descending:    return "rating_desc";
		case sort_rating_ascending:     return "rating_asc";
		default:                        return "";
	}
}

//------------------------------------------------------------------------
void Mootcher::setcUrlOptions()
{
	// some servers don't like requests that are made without a user-agent field, so we provide one
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0");
	// setup curl error buffer
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
	// Allow redirection
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);

	// Allow connections to time out (without using signals)
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30);
}

std::string Mootcher::doRequest(std::string uri, std::string params)
{
	std::string result;
	struct SfdbMemoryStruct xml_page;
	xml_page.memory = NULL;
	xml_page.size = 0;

	setcUrlOptions();
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &xml_page);

	// the url to get
	std::string url = base_url + uri + "?";
	if (params != "") {
		url += params + "&token=" + default_token + "&format=xml";
	} else {
		url += "token=" + default_token + "&format=xml";
	}

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str() );

	DEBUG_TRACE(PBD::DEBUG::Freesound, "doRequest() " + url + "\n");

	// perform online request
	CURLcode res = curl_easy_perform(curl);
	if( res != 0 ) {
		std::string errmsg = string_compose (_("curl error %1 (%2)"), res, curl_easy_strerror(res));
		error << errmsg << endmsg;
		DEBUG_TRACE(PBD::DEBUG::Freesound, errmsg + "\n");
		return "";
	}

	// free the memory
	if (xml_page.memory) {
		result = xml_page.memory;
	}

	free (xml_page.memory);
	xml_page.memory = NULL;
	xml_page.size = 0;

	DEBUG_TRACE(PBD::DEBUG::Freesound, result + "\n");
	return result;
}


std::string Mootcher::searchSimilar(std::string id)
{
	std::string params = "";

	params += "&fields=" + fields;
	params += "&num_results=100";
	// XXX should we filter out MP3s here, too?
	// XXX and what if there are more than 100 similar sounds?

	return doRequest("/sounds/" + id + "/similar/", params);
}

//------------------------------------------------------------------------

#if OAUTH_BUILTIN_HACK
class CredentialsDialog : public ArdourDialog
{

	public:
		CredentialsDialog(const std::string &title);
		const std::string username() { return username_entry.get_text(); }
		const std::string password() { return password_entry.get_text(); }
	private:
		Gtk::Label username_label;
		Gtk::Entry username_entry;
		Gtk::Label password_label;
		Gtk::Entry password_entry;
};

CredentialsDialog::CredentialsDialog(const std::string &title)
	: ArdourDialog (title, true)
	, username_label (_("User name:"))
	, password_label (_("Password:"))
{
	password_entry.set_visibility (false);
	get_vbox ()->pack_start (username_label);
	get_vbox ()->pack_start (username_entry);
	get_vbox ()->pack_start (password_label);
	get_vbox ()->pack_start (password_entry);

	username_label.set_alignment (Gtk::ALIGN_LEFT);
	password_label.set_alignment (Gtk::ALIGN_LEFT);

	username_entry.set_activates_default (true);
	password_entry.set_activates_default (true);

	add_button (Gtk::Stock::CANCEL, Gtk::RESPONSE_CANCEL);
	add_button (Gtk::Stock::OK,     Gtk::RESPONSE_ACCEPT);
	set_default_response (Gtk::RESPONSE_ACCEPT);

	show_all ();

}
#endif

void
Mootcher::report_login_error(const std::string &msg)
{
	DEBUG_TRACE(PBD::DEBUG::Freesound, "Login failed:" + msg + "\n");
	error << "Freesound login failed: " << msg << endmsg;
}

bool
Mootcher::get_oauth_token()
{
	/*
	 * Logging into Freesound requires us to jump through a few hoops.
	 * See http://www.freesound.org/docs/api/authentication.html#token-authentication for the
	 * documentation.
	 *
	 * First, we must retrieve the page at:
	 *     https://www.freesound.org/apiv2/oauth2/authorize/?client_id=c7eff9328525c51775cb&response_type=code
	 *
	 * Fortunately, this page is valid XHTML, so we can use XMLTree and friends to parse out
	 * the hidden form field valus that we'll need at the next step.
	 *
	 * Then, we must POST this page with the user's Freesound username and password, also
	 * passing along the values of "csrfmiddlewaretoken" and "next" that were passed to us in
	 * hidden form fields. "next" is the address of the next page that will be returned, which
	 * will be that of a page with an "authorize" button on.
	 *
	 * We must next POST this page (with the same csrfmiddlewaretoken value), and the name and
	 * value of the "authorize" button. Ideally, we'd parse out the <input ...> field of the
	 * "Authorize!" button and POST the value therein; unfortunately the page isn't valid
	 * XHTML, so we can't use XMLTree on it. For now, I don't even attempt to parse the page:
	 * I've assumed that the button is always named "authorize", with a value of "Authorize!".
	 *
	 * The returned page then contains an authorization code, which we have to exchange for a token,
	 * by POSTing to https://www.freesound.org/apiv2/oauth2/access_token/. This page expects
	 * POST parameters of client_id, client_secret, grant_type=authorization_code, & &code (= our authorization code)
	 *
	 * Again, this page isn't valid XML: for this one I've hacked up a parser that just looks
	 * for a <div> containing a 30character base64-ish string, which is what it presently returns.
	 *
	 * The returned page from this POST contains the token value which we should use for subsequent
	 * download requests, but (ha ha) this page is actually JSON, because the previous page (unlike
	 * all the other JSON pages on freesound.org) ignores the "&format=xml" parameter.
	 *
	 * The hard-won token then needs to be presented back to freesound.org as part of an
	 * "Authorization: Bearer " HTML header on all subsequent requests that require OAuth.
	 *
	 * This function, as you might expect from the foregoing description, is rather fragile
	 * in the face of any changes in the HTML that's served by freesound.org. Unfortunately, I
	 * can't see any better way of doing this without them extending their API to make it less
	 * convoluted for non-web apps to log in.
	 *
	 * Other possibilities I considered include doing one or more of:
	 *     - use HTMLTree and friends from libxml2 in place of XMLTree to parse the HTML pages.
	 *     - use libtidy or similar to sanitise the HTML before feeding it to XMLTree.
	 *     - just pop up a link in Ardour when we require the user to log into Freesound,
	 *       and provide a place for them to manually paste the access token.
	 *     - embed a mini-browser into Ardour, and use that to show the Freesound login and token pages.
	 *     - try to persuade Freesound to change their API, or at least send valid XHTML.
	 *     - dropping support for direct freesound import into Ardour altogether.
	 *     - opening a link to the freesound.org authentication page in the user's browser,
	 *       for them to log in, and copy-&-paste the authorization code from there into Ardour.
	 *
	 */

	CURLcode res;
	struct SfdbMemoryStruct xml_page;
	xml_page.memory = NULL;
	xml_page.size = 0;
	std::string auth_code = "";

	std::string oauth_url = base_url + "/oauth2/logout_and_authorize/?client_id="+client_id+"&response_type=code&state=hello";

	setcUrlOptions();
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &xml_page);

#if !OAUTH_BUILTIN_HACK


	PBD::open_uri (oauth_url);
	ArdourWidgets::Prompter token_entry(true);
	token_entry.set_prompt(_("Paste Freesound authorization code"));
	token_entry.set_title(_("Authorization Code"));

	token_entry.set_name ("TokenEntryWindow");
	// token_entry.set_size_request (250, -1);
	token_entry.set_position (Gtk::WIN_POS_MOUSE);
	token_entry.add_button (Gtk::Stock::OK, Gtk::RESPONSE_ACCEPT);
	token_entry.show ();

	if (token_entry.run () != Gtk::RESPONSE_ACCEPT)
		return false;

	token_entry.get_result(auth_code);
	if (auth_code == "")
		return false;

	//XXX any other checks required/possible?

	// We don't need to set the "Authorization:" header here because the instance of
	// curl in this mootcher is still logged in. Subsequently created mootchers with
	// the token passed into their constructors will have the header set there.

#else
	XMLTree doc;

	CredentialsDialog freesound_credentials(_("Enter Freesound user name & password"));
	int r = freesound_credentials.run();
	freesound_credentials.hide();
	if (r != Gtk::RESPONSE_ACCEPT) {
		return false;
	}
	while (gtk_events_pending()) {
		// allow the dialogue to become hidden
		gtk_main_iteration ();
	}

	std::string username = freesound_credentials.username();
	std::string password = freesound_credentials.password();

	DEBUG_TRACE(PBD::DEBUG::Freesound, "get_oauth_token(" + username + ",*****)\n");
	std::string cookie_file = Glib::build_filename (ARDOUR::user_config_directory(), "freesound-cookies");
	std::string oauth_page_str;

	curl_easy_setopt(curl, CURLOPT_URL, oauth_url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookie_file.c_str());
	if (DEBUG_ENABLED(PBD::DEBUG::Freesound)) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	}
	curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANYSAFE);

	progress_bar.set_text(_("Connecting to Freesound.org login page..."));
	while (gtk_events_pending()) gtk_main_iteration (); // allow the progress bar text to update

	res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		if (res != CURLE_ABORTED_BY_CALLBACK) {
			report_login_error (string_compose ("curl failed: %1, error=%2", oauth_url, res));
		}
		return false;
	}
	if (!xml_page.memory) {
		report_login_error (string_compose ("curl returned nothing, url=%1!", oauth_url));
		return false;
	}

	oauth_page_str = xml_page.memory;
	free (xml_page.memory);
	xml_page.memory = NULL;
	xml_page.size = 0;

	//XXX transform to XHTML
	size_t ap = oauth_page_str.find("<body>");
	if (ap == std::string::npos) {
		report_login_error ("no <body> found in login page");
		return false;
	}
	while ((ap = oauth_page_str.find(" autofocus ", ap)) != std::string::npos) {
		oauth_page_str.insert(ap+10, "=\"autofocus\"");
		ap += 22;
	}
	ap = oauth_page_str.find("<body>");
	while ((ap = oauth_page_str.find(" required ", ap)) != std::string::npos) {
		oauth_page_str.insert(ap+9, "=\"required\"");
		ap += 20;
	}

	DEBUG_TRACE(PBD::DEBUG::Freesound, oauth_page_str);
	doc.read_buffer(oauth_page_str.c_str());
	XMLNode *oauth_page = doc.root();
	if (!oauth_page) {
		report_login_error ("oauth_page page has no doc.root!");
		return false;
	}
	if (strcasecmp (doc.root()->name().c_str(), "html")) {
		report_login_error ("root is not <html>");
		return false;
	}

	// find all input fields with both name and value properties (there should be at least two of them, for
	// csrfmiddlewaretoken and next page).
	boost::shared_ptr<XMLSharedNodeList> inputs = doc.find("//input[@name and @value]", oauth_page);
	DEBUG_TRACE(PBD::DEBUG::Freesound, string_compose("found %1 input nodes\n", inputs->size()));

	std::string csrf_mwt = "", next_page = "";
	for (XMLSharedNodeList::const_iterator i = inputs->begin(); i != inputs->end(); ++i) {
		XMLProperty *prop_name  = (*i)->property("name");
		XMLProperty *prop_value = (*i)->property("value");
		if (prop_name && prop_value) {
			std::string input_name  = prop_name->value();
			std::string input_value = prop_value->value();
				DEBUG_TRACE(PBD::DEBUG::Freesound,
						string_compose("found input name %1, value = %2\n", input_name, input_value));
			if (input_name == "csrfmiddlewaretoken") {
				csrf_mwt  = input_value;
			} else if (input_name == "next") {
				next_page = input_value;
			}
		}
	}

	if (csrf_mwt =="") {
		report_login_error ("csrfmiddlewaretoken not found");
		return false;
	}

	if (next_page =="") {
		report_login_error ("next page not found");
		return false;
	}

	DEBUG_TRACE(PBD::DEBUG::Freesound, "\n\n*** about to log in...***\n\n");

	char *next_escaped = curl_easy_escape(curl, next_page.c_str(), 0);
	DEBUG_TRACE(PBD::DEBUG::Freesound, string_compose("next_escaped: %1\n\n", next_escaped));

	curl_easy_setopt(curl, CURLOPT_POST, 4);
	curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS,
			( "csrfmiddlewaretoken=" + csrf_mwt +
			  "&username=" + username +
			  "&password=" + password +
			  "&next=" + next_escaped).c_str());

	curl_free (next_escaped);
	curl_easy_setopt(curl, CURLOPT_REFERER, oauth_url.c_str());

	/* POST the login form */
	DEBUG_TRACE(PBD::DEBUG::Freesound, "*** posting... ***\n\n");
	progress_bar.set_text(_("Logging in to Freesound.org..."));
	while (gtk_events_pending()) gtk_main_iteration (); // allow the progress bar text to update

	res = curl_easy_perform (curl);
	if (res != CURLE_OK) {
		if (res != CURLE_ABORTED_BY_CALLBACK) {
			report_login_error (string_compose ("curl failed: %1, error=%2", oauth_url, res));
		}
		return false;
	}

	if (!xml_page.memory) {
		report_login_error (string_compose ("curl returned nothing, url=%1!", oauth_url));
		return false;
	}

	oauth_page_str = xml_page.memory;
	free (xml_page.memory);
	xml_page.memory = NULL;
	xml_page.size = 0;

	DEBUG_TRACE(PBD::DEBUG::Freesound, oauth_page_str);
#if FREESOUND_EVER_SENDS_VALID_XML
	if (!doc.read_buffer (oauth_page_str.c_str())) {
		report_login_error ("doc.read_buffer() returns false");
		return false;
	}
	XMLNode *authorize_page = doc.root();
	if (!authorize_page) {
		report_login_error ("authorize page has no doc.root!");
		return false;
	}

	authorize_page->dump(std::cerr, "authorize page:");

	// find input fields with name, value, & type properties, i.e. the 'Authorize' button
	boost::shared_ptr<XMLSharedNodeList> buttons = doc.find("//input[@name and @value and @type]", oauth_page);

	std::cerr << "found " << buttons->size() << " buttons\n";

	bool found_auth_button = false;
	for (XMLSharedNodeList::const_iterator i = buttons->begin(); i != buttons->end(); ++i) {
		XMLProperty *prop_name  = (*i)->property("name");
		XMLProperty *prop_value = (*i)->property("value");
		XMLProperty *prop_type  = (*i)->property("type");
		if (prop_name && prop_value) {
			std::string input_name  = prop_name->value();
			std::string input_value = prop_value->value();
			std::string input_type  = prop_type->value();
			std::cerr << "found input name :" << input_name << ", value = " << input_value << std::endl;
			if (input_name == "authorize" && input_type == "submit") {
				char *input_value_escaped = curl_easy_escape(curl, input_value.c_str(), 0);
				curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS,
						("csrfmiddlewaretoken=" + csrf_mwt +
						 "&" + input_name + "=" + input_value_escaped).c_str());
				curl_free (input_value_escaped);
				found_auth_button = true;
				break;
			}
		}
	}
	if (!found_auth_button) {
		report_login_error ("'authorize' button not found");
		return false;
	}

#else
	// hard-code the name & value of the "Authorize!" button
	curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, ("csrfmiddlewaretoken=" + csrf_mwt + "&authorize=Authorize%21").c_str());

#endif

	curl_easy_setopt(curl, CURLOPT_POST, 2);

	/* POST the "Authorize!" form */
	progress_bar.set_text(string_compose(_("Authorising %1 to access Freesound.org..."), PROGRAM_NAME));
	while (gtk_events_pending()) gtk_main_iteration (); // allow the progress bar text to update

	res = curl_easy_perform (curl);
	if (res != CURLE_OK) {
		if (res != CURLE_ABORTED_BY_CALLBACK) {
			report_login_error (string_compose ("curl failed: %1, error=%2", oauth_url, res));
		}
		return false;
	}

	if (!xml_page.memory) {
		report_login_error (string_compose ("curl returned nothing, url=%1!", oauth_url));
		return false;
	}

	oauth_page_str = xml_page.memory;
	free (xml_page.memory);
	xml_page.memory = NULL;
	xml_page.size = 0;

	DEBUG_TRACE(PBD::DEBUG::Freesound, oauth_page_str);

	static const char *base64ish = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	static const int base64len = 30;

#if FREESOUND_EVER_SENDS_VALID_XML

	if (!doc.read_buffer (oauth_page_str.c_str())) {
		report_login_error ("doc.read_buffer() of token page returns false");
		return false;
	}
	XMLNode *auth_granted_page = doc.root();
	if (!auth_granted_page) {
		report_login_error ("auth_granted_page has no doc.root!");
		return false;
	}

	auth_granted_page->dump(std::cerr, "auth granted page:");

	// find input fields with name, value, & type properties
	boost::shared_ptr<XMLSharedNodeList> codez = doc.find("//div[@style]", oauth_page);

	std::cerr << "found " << codez->size() << " codez\n";

	for (XMLSharedNodeList::const_iterator i = codez->begin(); i != codez->end(); ++i) {
		XMLProperty *prop_style = (*i)->property("style");
		const std::string content = (*i)->content();

		if (prop_style && content.length() == base64len) {
			size_t p = content.find_first_not_of(base64ish);
			if (p == std::string::npos) {
				oauth_token = content;
				return true;
			}
		}
	}
#else
	// hackily parse through the HTML looking for a <div> tag with base64len-character base64-ish contents
	size_t p = 0;

	while (true) {
		DEBUG_TRACE(PBD::DEBUG::Freesound, string_compose("searching for \"<div \" from %1\n", p));
		p = oauth_page_str.find("<div ", p);
		if (p == std::string::npos) {
			break;
		}
		DEBUG_TRACE(PBD::DEBUG::Freesound, string_compose("searching for \">\" from %1\n", p));
		size_t q = oauth_page_str.find(">", p);
		if (q == std::string::npos) {
			break;
		}
		DEBUG_TRACE(PBD::DEBUG::Freesound, string_compose("searching for \"</div>\" from %1\n", q));
		size_t r = oauth_page_str.find("</div>", q);
		if (r == std::string::npos) {
			break;
		}
		p = q + 1;
		DEBUG_TRACE(PBD::DEBUG::Freesound, string_compose("checking content length: %1 - %2 = %3\n", r, q, r - q));
		if (r - q != base64len + 1) {
			continue;
		}
		std::string content = oauth_page_str.substr(q + 1, base64len);
		DEBUG_TRACE(PBD::DEBUG::Freesound, string_compose("checking content is base64ish: %1\n", content));
		if (content.find_first_not_of(base64ish) != std::string::npos) {
			continue;
		}
		DEBUG_TRACE(PBD::DEBUG::Freesound, "Got authorization code!\n");
		auth_code = content;
		break;
	}
#endif

	if (auth_code == "") {
		report_login_error ("Failed to get authorization code!");
		return false;
	}

#endif

	curl_easy_setopt(curl, CURLOPT_URL, (base_url + "/oauth2/access_token/").c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 5);
	curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS,
			("client_id=" + client_id +
			"&client_secret=" + default_token +
			"&grant_type=authorization_code" +
			"&code=" + auth_code).c_str());

	progress_bar.set_text(_("Fetching Access Token..."));
	while (gtk_events_pending()) gtk_main_iteration (); // allow the progress bar text to update

	res = curl_easy_perform (curl);
	if (res != CURLE_OK) {
		if (res != CURLE_ABORTED_BY_CALLBACK) {
			report_login_error (string_compose ("curl failed: %1, error=%2", oauth_url, res));
		}
		return false;
	}

	if (!xml_page.memory) {
		report_login_error (string_compose ("curl returned nothing, url=%1!", oauth_url));
		return false;
	}

	std::string access_token_json_str = xml_page.memory;
	free (xml_page.memory);
	xml_page.memory = NULL;
	xml_page.size = 0;

	DEBUG_TRACE(PBD::DEBUG::Freesound, access_token_json_str);

	// one of these days ardour's gonna need a proper JSON parser...
	size_t token_pos = access_token_json_str.find ("access_token");
	oauth_token = access_token_json_str.substr (token_pos + 16, 30);

	// we've set a bunch of curl options - reset the important ones now
	curl_easy_setopt(curl, CURLOPT_POST, 0);

	DEBUG_TRACE(PBD::DEBUG::Freesound, "oauth_token is :" + oauth_token + "\n");
	return true;

}


std::string Mootcher::searchText(std::string query, int page, std::string filter, enum sortMethod sort)
{
	std::string params = "";
	char buf[24];

	if (page > 1) {
		snprintf(buf, 23, "page=%d&", page);
		params += buf;
	}

	char *eq = curl_easy_escape(curl, query.c_str(), query.length());
	params += "query=\"" + std::string(eq) + "\"";
	curl_free(eq);

	if (filter != "") {
		char *ef = curl_easy_escape(curl, filter.c_str(), filter.length());
		params += "&filter=" + std::string(ef);
		curl_free(ef);
	}

	if (sort)
		params += "&sort=" + sortMethodString(sort);

	params += "&fields=" + fields;
	params += "&page_size=100";

	return doRequest("/search/text/", params);
}

//------------------------------------------------------------------------

std::string Mootcher::getSoundResourceFile(std::string ID)
{

	std::string originalSoundURI;
	std::string audioFileName;
	std::string xml;

	DEBUG_TRACE(PBD::DEBUG::Freesound, "getSoundResourceFile(" + ID + ")\n");

	// download the xmlfile into xml_page
	xml = doRequest("/sounds/" + ID + "/", "");

	XMLTree doc;
	doc.read_buffer( xml.c_str() );
	XMLNode *freesound = doc.root();

	// if the page is not a valid xml document with a 'freesound' root
	if (freesound == NULL) {
		error << _("getSoundResourceFile: There is no valid root in the xml file") << endmsg;
		return "";
	}

	if (strcmp(doc.root()->name().c_str(), "root") != 0) {
		error << string_compose (_("getSoundResourceFile: root = %1, != \"root\""), doc.root()->name()) << endmsg;
		return "";
	}

	XMLNode *name = freesound->child("name");

	// get the file name and size from xml file
	if (name) {

		audioFileName = Glib::build_filename (basePath, ID + "-" + name->child("text")->content());

		//store all the tags in the database
		XMLNode *tags = freesound->child("tags");
		if (tags) {
			XMLNodeList children = tags->children();
			XMLNodeConstIterator niter;
			std::vector<std::string> strings;
			for (niter = children.begin(); niter != children.end(); ++niter) {
				XMLNode *node = *niter;
				if( strcmp( node->name().c_str(), "list-item") == 0 ) {
					XMLNode *text = node->child("text");
					if (text) {
						// std::cerr << "tag: " << text->content() << std::endl;
						strings.push_back(text->content());
					}
				}
			}
			ARDOUR::Library->set_tags (std::string("//")+audioFileName, strings);
			ARDOUR::Library->save_changes ();
		}
	}

	return audioFileName;
}

int audioFileWrite(void *buffer, size_t size, size_t nmemb, void *file)
{
	return (int)fwrite(buffer, size, nmemb, (FILE*) file);
};

//------------------------------------------------------------------------

void *
Mootcher::threadFunc() {
CURLcode res;

	DEBUG_TRACE(PBD::DEBUG::Freesound, "threadFunc\n");
	res = curl_easy_perform (curl);
	fclose (theFile);
	curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 1); // turn off the progress bar

	if (res != CURLE_OK) {
		/* it's not an error if the user pressed the stop button */
		if (res != CURLE_ABORTED_BY_CALLBACK) {
			error <<  string_compose (_("curl error %1 (%2)"), res, curl_easy_strerror(res)) << endmsg;
		}
		remove ( (audioFileName+".part").c_str() );
	} else {
		rename ( (audioFileName+".part").c_str(), audioFileName.c_str() );
		// now download the tags &c.
		getSoundResourceFile(ID);
	}

	return (void *) res;
}

void
Mootcher::doneWithMootcher()
{

	// update the sound info pane if the selection in the list box is still us
	sfb->refresh_display(ID, audioFileName);

	delete this; // this should be OK to do as long as Progress and Finished signals are always received in the order in which they are emitted
}

static void *
freesound_download_thread_func(void *arg)
{
	Mootcher *thisMootcher = (Mootcher *) arg;
	void *res;

	// std::cerr << "freesound_download_thread_func(" << arg << ")" << std::endl;
	res = thisMootcher->threadFunc();

	thisMootcher->Finished(); /* EMIT SIGNAL */
	return res;
}


//------------------------------------------------------------------------

bool Mootcher::checkAudioFile(std::string originalFileName, std::string theID)
{
	DEBUG_TRACE(PBD::DEBUG::Freesound, string_compose("checkAudiofile(%1, %2)\n", originalFileName, theID));
	ensureWorkingDir();
	ID = theID;
	audioFileName = Glib::build_filename (basePath, ID + "-" + originalFileName);

	// check to see if audio file already exists
	FILE *testFile = g_fopen(audioFileName.c_str(), "r");
	if (testFile) {
		fseek (testFile , 0 , SEEK_END);
		if (ftell (testFile) > 256) {
			fclose (testFile);
			DEBUG_TRACE(PBD::DEBUG::Freesound, "checkAudiofile() - found " + audioFileName + "\n");
			return true;
		}

		// else file was small, probably an error, delete it
		DEBUG_TRACE(PBD::DEBUG::Freesound, "checkAudiofile() - " + audioFileName + " <= 256 bytes, removing it\n");
		fclose (testFile);
		remove (audioFileName.c_str() );
	}
	DEBUG_TRACE(PBD::DEBUG::Freesound, "checkAudiofile() - not found " + audioFileName + "\n");
	return false;
}


const std::string
Mootcher::fetchAudioFile(std::string originalFileName, std::string theID, std::string audioURL, SoundFileBrowser *caller)
{

	DEBUG_TRACE(PBD::DEBUG::Freesound, string_compose("fetchAudiofile(%1, %2, %3, ...)\n", originalFileName, theID, audioURL));

	ensureWorkingDir();
	ID = theID;
	audioFileName = Glib::build_filename (basePath, ID + "-" + originalFileName);

	if (!curl) {
		return "";
	}

	Gtk::VBox *freesound_vbox = dynamic_cast<Gtk::VBox *> (caller->notebook.get_nth_page(2));
	freesound_vbox->pack_start(progress_hbox, Gtk::PACK_SHRINK);

	cancel_download = false;
	curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 0); // turn on the progress bar
	curl_easy_setopt (curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
	curl_easy_setopt (curl, CURLOPT_PROGRESSDATA, this);

	if (oauth_token == "") {
		if (!get_oauth_token()) {
			return "";
		}
	}

	// now download the actual file
	theFile = g_fopen( (audioFileName + ".part").c_str(), "wb" );

	if (!theFile) {
		DEBUG_TRACE(PBD::DEBUG::Freesound, "Can't open file for writing:" + audioFileName + ".part\n");
		return "";
	}

	// create the download url
	audioURL += "?token=" + default_token;

	setcUrlOptions();
	curl_easy_setopt(curl, CURLOPT_URL, audioURL.c_str() );
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, audioFileWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, theFile);

	std::string prog;
	prog = string_compose (_("%1"), originalFileName);
	progress_bar.set_text(prog);
	progress_hbox.show();

	sfb = caller;

	Progress.connect(*this, invalidator (*this), boost::bind(&Mootcher::updateProgress, this, _1, _2), gui_context());
	Finished.connect(*this, invalidator (*this), boost::bind(&Mootcher::doneWithMootcher, this), gui_context());
	pthread_t freesound_download_thread;
	pthread_create_and_store("freesound_import", &freesound_download_thread, freesound_download_thread_func, this);

	return oauth_token;
}

//---------

void
Mootcher::updateProgress(double dlnow, double dltotal)
{
	if (dltotal > 0) {
		double fraction = dlnow / dltotal;
		// std::cerr << "progress idle: " << progress_bar.get_text() << ". " << dlnow << " / " << dltotal << " = " << fraction << std::endl;
		if (fraction > 1.0) {
			fraction = 1.0;
		} else if (fraction < 0.0) {
			fraction = 0.0;
		}
		progress_bar.set_fraction(fraction);
	}
}

int
Mootcher::progress_callback(void *caller, double dltotal, double dlnow, double /*ultotal*/, double /*ulnow*/)
{
	// It may seem curious to pass a pointer to an instance of an object to a static
	// member function, but we can't use a normal member function as a curl progress callback,
	// and we want access to some private members of Mootcher.

	Mootcher *thisMootcher = (Mootcher *) caller;

	if (thisMootcher->cancel_download) {
		return -1;
	}

	thisMootcher->Progress(dlnow, dltotal); /* EMIT SIGNAL */
	return 0;
}

