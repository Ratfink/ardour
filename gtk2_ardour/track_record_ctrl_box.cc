/*
 * Copyright (C) 2020 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <list>

#include <sigc++/bind.h>

#include "pbd/unwind.h"

#include "ardour/logmeter.h"
#include "ardour/meter.h"
#include "ardour/route.h"
#include "ardour/route_group.h"
#include "ardour/session.h"

#include "ardour/audio_track.h"
#include "ardour/midi_track.h"

#include "gtkmm2ext/gtk_ui.h"
#include "gtkmm2ext/keyboard.h"
#include "gtkmm2ext/rgb_macros.h"
#include "gtkmm2ext/utils.h"

#include "widgets/tooltips.h"

#include "ardour_window.h"
#include "context_menu_helper.h"
#include "gui_thread.h"
#include "level_meter.h"
#include "meter_patterns.h"
#include "route_group_menu.h"
#include "ui_config.h"
#include "utils.h"

#include "track_record_ctrl_box.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace ArdourMeter;
using namespace ArdourWidgets;
using namespace ARDOUR_UI_UTILS;
using namespace PBD;
using namespace Gtk;
using namespace Gtkmm2ext;
using namespace std;

PBD::Signal1<void, TrackRecordControlBox*> TrackRecordControlBox::CatchDeletion;

#define PX_SCALE(pxmin, dflt) rint (std::max ((double)pxmin, (double)dflt* UIConfiguration::instance ().get_ui_scale ()))

Glib::RefPtr<Gtk::SizeGroup> TrackRecordControlBox::_track_number_size_group = Gtk::SizeGroup::create (Gtk::SIZE_GROUP_BOTH);
Glib::RefPtr<Gtk::SizeGroup> TrackRecordControlBox::_level_meters_size_group = Gtk::SizeGroup::create (Gtk::SIZE_GROUP_BOTH);

TrackRecordControlBox::TrackRecordControlBox (Session* s, boost::shared_ptr<ARDOUR::Route> rt)
	: SessionHandlePtr (s)
	, RouteUI (s)
	, _clear_meters (true)
	, _route_group_menu (0)
	, _route_group_button (S_("RTAV|G"))
	, _ctrls_button_size_group (Gtk::SizeGroup::create (Gtk::SIZE_GROUP_BOTH))
{
	RouteUI::set_route (rt);

	_route->DropReferences.connect (_route_connections, invalidator (*this), boost::bind (&TrackRecordControlBox::self_delete, this), gui_context ());

	UI::instance ()->theme_changed.connect (sigc::mem_fun (*this, &TrackRecordControlBox::on_theme_changed));
	UIConfiguration::instance ().ColorsChanged.connect (sigc::mem_fun (*this, &TrackRecordControlBox::on_theme_changed));
	UIConfiguration::instance ().DPIReset.connect (sigc::mem_fun (*this, &TrackRecordControlBox::on_theme_changed));
	UIConfiguration::instance ().ParameterChanged.connect (sigc::mem_fun (*this, &TrackRecordControlBox::parameter_changed));

	Config->ParameterChanged.connect (*this, invalidator (*this), ui_bind (&TrackRecordControlBox::parameter_changed, this, _1), gui_context ());
	s->config.ParameterChanged.connect (*this, invalidator (*this), ui_bind (&TrackRecordControlBox::parameter_changed, this, _1), gui_context ());

	ResetAllPeakDisplays.connect (sigc::mem_fun (*this, &TrackRecordControlBox::reset_peak_display));
	ResetRoutePeakDisplays.connect (sigc::mem_fun (*this, &TrackRecordControlBox::reset_route_peak_display));
	ResetGroupPeakDisplays.connect (sigc::mem_fun (*this, &TrackRecordControlBox::reset_group_peak_display));

	_number_label.set_name ("tracknumber label");
	_number_label.set_elements ((ArdourButton::Element) (ArdourButton::Edge | ArdourButton::Body | ArdourButton::Text | ArdourButton::Inactive));
	_number_label.set_alignment (.5, .5);

	PropertyList* plist = new PropertyList();
	plist->add (ARDOUR::Properties::group_mute, true);
	plist->add (ARDOUR::Properties::group_solo, true);
	_route_group_menu = new RouteGroupMenu (_session, plist);

	_route_group_button.set_name ("route button");
	_route_group_button.signal_button_press_event().connect (sigc::mem_fun(*this, &TrackRecordControlBox::route_group_click), false);

	_level_meter = new LevelMeterHBox (s);
	_level_meter->set_meter (_route->shared_peak_meter ().get ());
	_level_meter->clear_meters ();
	_level_meter->setup_meters (50);

	name_label.set_name (X_("TrackNameEditor"));
	name_label.set_alignment (0.0, 0.5);
	name_label.set_width_chars (12);

	parameter_changed ("editor-stereo-only-meters");
	parameter_changed ("time-axis-name-ellipsize-mode");

	_ctrls.attach (_number_label,         0, 1, 0, 2, Gtk::SHRINK, Gtk::FILL,   4, 0);
	_ctrls.attach (*rec_enable_button,    1, 2, 0, 1, Gtk::SHRINK, Gtk::SHRINK, 0, 0);
	_ctrls.attach (*monitor_input_button, 2, 3, 0, 1, Gtk::SHRINK, Gtk::SHRINK, 0, 0);
	_ctrls.attach (*monitor_disk_button,  3, 4, 0, 1, Gtk::SHRINK, Gtk::SHRINK, 0, 0);
	_ctrls.attach (*mute_button,          4, 5, 0, 1, Gtk::SHRINK, Gtk::SHRINK, 0, 0);
	_ctrls.attach (name_label,            1, 4, 1, 2, Gtk::FILL,   Gtk::SHRINK, 0, 0);
	_ctrls.attach (_route_group_button,   4, 5, 1, 2, Gtk::SHRINK, Gtk::SHRINK, 0, 0);
	_ctrls.attach (*_level_meter,         5, 6, 0, 2, Gtk::SHRINK, Gtk::FILL,   4, 0);

	set_tooltip (*mute_button, _("Mute"));
	set_tooltip (*rec_enable_button, _("Record"));
	set_tooltip (_route_group_button, _("Group"));

	set_name_label ();
	update_sensitivity ();

	_level_meters_size_group->add_widget (*_level_meter);
	_track_number_size_group->add_widget (_number_label);
	_ctrls_button_size_group->add_widget (*rec_enable_button);
	_ctrls_button_size_group->add_widget (*mute_button);
	_ctrls_button_size_group->add_widget (*monitor_input_button);
	_ctrls_button_size_group->add_widget (*monitor_disk_button);
	_ctrls_button_size_group->add_widget (_route_group_button);

	_frame.add (_ctrls);
	pack_start (_frame, false, false);

	rec_enable_button->show ();
	monitor_input_button->show ();
	monitor_disk_button->show ();
	mute_button->show ();
	_level_meter->show ();
	_route_group_button.show();
	_number_label.show ();
	name_label.show ();
	_ctrls.show ();
	_frame.show ();
}

TrackRecordControlBox::~TrackRecordControlBox ()
{
	delete _level_meter;
	delete _route_group_menu;
	CatchDeletion (this);
}

void
TrackRecordControlBox::self_delete ()
{
	delete this;
}

void
TrackRecordControlBox::set_session (Session* s)
{
	RouteUI::set_session (s);
	if (!s) {
		return;
	}
	s->config.ParameterChanged.connect (*this, invalidator (*this), ui_bind (&TrackRecordControlBox::parameter_changed, this, _1), gui_context ());
}

void
TrackRecordControlBox::blink_rec_display (bool onoff)
{
	RouteUI::blink_rec_display (onoff);
}

std::string
TrackRecordControlBox::state_id () const
{
	if (_route) {
		return string_compose ("recctrl %1", _route->id ().to_s ());
	} else {
		return string ();
	}
}

void
TrackRecordControlBox::set_button_names ()
{
	mute_button->set_text (S_("Mute|M"));
#if 0
	monitor_input_button->set_text (S_("MonitorInput|I"));
	monitor_disk_button->set_text (S_("MonitorDisk|D"));
#else
	monitor_input_button->set_text (_("In"));
	monitor_disk_button->set_text (_("Disk"));
#endif

	/* Solo/Listen is N/A */
}

void
TrackRecordControlBox::route_property_changed (const PropertyChange& what_changed)
{
	if (!what_changed.contains (ARDOUR::Properties::name)) {
		return;
	}
	ENSURE_GUI_THREAD (*this, &TrackRecordControlBox::route_property_changed, what_changed);
	set_name_label ();
	set_tooltip (*_level_meter, _route->name ());
}

void
TrackRecordControlBox::route_color_changed ()
{
	_number_label.set_fixed_colors (gdk_color_to_rgba (color ()), gdk_color_to_rgba (color ()));
}

void
TrackRecordControlBox::on_theme_changed ()
{
}

void
TrackRecordControlBox::on_size_request (Gtk::Requisition* r)
{
	VBox::on_size_request (r);
}

void
TrackRecordControlBox::on_size_allocate (Gtk::Allocation& a)
{
	VBox::on_size_allocate (a);
}

void
TrackRecordControlBox::parameter_changed (std::string const& p)
{
	if (p == "editor-stereo-only-meters") {
		if (UIConfiguration::instance ().get_editor_stereo_only_meters ()) {
			_level_meter->set_max_audio_meter_count (2);
		} else {
			_level_meter->set_max_audio_meter_count (0);
		}
	} else if (p == "time-axis-name-ellipsize-mode") {
		set_name_ellipsize_mode ();
	}
}

string
TrackRecordControlBox::name () const
{
	return _route->name ();
}

Gdk::Color
TrackRecordControlBox::color () const
{
	return RouteUI::route_color ();
}

void
TrackRecordControlBox::set_name_label ()
{
	string x = _route->name ();
	if (x != name_label.get_text ()) {
		name_label.set_text (x);
	}
	set_tooltip (name_label, _route->name ());

	const int64_t track_number = _route->track_number ();
	assert (track_number > 0);
	_number_label.set_text (PBD::to_string (track_number));
}

void
TrackRecordControlBox::route_active_changed ()
{
	RouteUI::route_active_changed ();
	update_sensitivity ();
}

void
TrackRecordControlBox::update_sensitivity ()
{
	bool en = _route->active ();
	monitor_input_button->set_sensitive (en);
	monitor_disk_button->set_sensitive (en);
	_ctrls.set_sensitive (en);
}

void
TrackRecordControlBox::fast_update ()
{
	if (_clear_meters) {
		_level_meter->clear_meters ();
		_clear_meters = false;
	}
	_level_meter->update_meters ();
}

void
TrackRecordControlBox::reset_route_peak_display (Route* route)
{
	if (_route && _route.get () == route) {
		reset_peak_display ();
	}
}

void
TrackRecordControlBox::reset_group_peak_display (RouteGroup* group)
{
	if (_route && group == _route->route_group ()) {
		reset_peak_display ();
	}
}

void
TrackRecordControlBox::reset_peak_display ()
{
	_route->shared_peak_meter ()->reset_max ();
	_clear_meters = true;
}

bool
TrackRecordControlBox::route_group_click (GdkEventButton* ev)
{
	if (Keyboard::modifier_state_equals (ev->state, Keyboard::PrimaryModifier)) {
		if (_route->route_group()) {
			_route->route_group()->remove (_route);
		}
		return false;
	}

	WeakRouteList r;
	r.push_back (route ());
	_route_group_menu->build (r);

	Gtkmm2ext::anchored_menu_popup (_route_group_menu->menu(), &_route_group_button, "", 1, ev->time);

	return true;
}
