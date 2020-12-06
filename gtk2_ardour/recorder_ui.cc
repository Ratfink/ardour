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

#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include <algorithm>

#include "ardour/audioengine.h"
#include "ardour/dB.h"
#include "ardour/logmeter.h"
#include "ardour/port_manager.h"
#include "ardour/session.h"

#include "gtkmm2ext/bindings.h"
#include "gtkmm2ext/gtk_ui.h"
#include "gtkmm2ext/keyboard.h"
#include "gtkmm2ext/utils.h"
#include "gtkmm2ext/window_title.h"

#include "widgets/fastmeter.h"
#include "widgets/tooltips.h"

#include "actions.h"
#include "ardour_ui.h"
#include "gui_thread.h"
#include "recorder_ui.h"
#include "timers.h"
#include "track_record_ctrl_box.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace Gtkmm2ext;
using namespace ArdourWidgets;
using namespace Gtk;
using namespace std;

RecorderUI::RecorderUI ()
	: Tabbable (_content, _("Recorder"), X_("recorder"))
	, _rec_box_width (220)
	, _rec_area_cols (2)
{
	load_bindings ();
	register_actions ();

	_meter_scroller.add (_meter_area);
	_meter_area.set_spacing (2);

	_rec_scroller.add (_rec_area);
	_rec_scroller.set_shadow_type(Gtk::SHADOW_NONE);
	_rec_scroller.set_policy (Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);

	_rec_area.set_spacing (0);
	_rec_area.pack_start (_rec_table, false, false);
	_rec_area.signal_size_request().connect (sigc::mem_fun (*this, &RecorderUI::rec_area_size_request));
	_rec_area.signal_size_allocate ().connect (mem_fun (this, &RecorderUI::rec_area_size_allocate));

	_pane.add (_rec_scroller);
	_pane.add (_meter_scroller);

	_content.pack_start (_pane, true, true);
	_content.set_data ("ardour-bindings", bindings);

	update_title ();

	_rec_table.show ();
	_rec_area.show ();
	_rec_scroller.show ();
	_meter_area.show ();
	_meter_scroller.show ();
	_pane.show ();
	_content.show ();

	AudioEngine::instance ()->Running.connect (_engine_connections, invalidator (*this), boost::bind (&RecorderUI::start_updating, this), gui_context ());
	AudioEngine::instance ()->Stopped.connect (_engine_connections, invalidator (*this), boost::bind (&RecorderUI::stop_updating, this), gui_context ());
	AudioEngine::instance ()->Halted.connect (_engine_connections, invalidator (*this), boost::bind (&RecorderUI::stop_updating, this), gui_context ());
	AudioEngine::instance ()->PortConnectedOrDisconnected.connect (_engine_connections, invalidator (*this), boost::bind (&RecorderUI::port_connected_or_disconnected, this, _2, _4), gui_context ());

	//ARDOUR_UI::instance()->Escape.connect (*this, invalidator (*this), boost::bind (&RecorderUI::escape, this), gui_context());

	PresentationInfo::Change.connect (*this, invalidator (*this), boost::bind (&RecorderUI::presentation_info_changed, this, _1), gui_context());

	XMLNode const* settings = ARDOUR_UI::instance()->recorder_settings();
	float          fract;

	if (!settings || !settings->get_property ("recorder-vpane-pos", fract) || fract > 1.0) {
		fract = 0.75f;
	}
	_pane.set_divider (0, fract);
}

RecorderUI::~RecorderUI ()
{
}

void
RecorderUI::cleanup ()
{
	stop_updating ();
	_engine_connections.drop_connections ();
}

Gtk::Window*
RecorderUI::use_own_window (bool and_fill_it)
{
	bool new_window = !own_window ();

	Gtk::Window* win = Tabbable::use_own_window (and_fill_it);

	if (win && new_window) {
		win->set_name ("RecorderWindow");
		ARDOUR_UI::instance ()->setup_toplevel_window (*win, _("Recorder"), this);
		win->signal_event ().connect (sigc::bind (sigc::ptr_fun (&Keyboard::catch_user_event_for_pre_dialog_focus), win));
		win->set_data ("ardour-bindings", bindings);
		update_title ();
#if 0 // TODO
		if (!win->get_focus()) {
			win->set_focus (scroller);
		}
#endif
	}

	//contents ().show_all ();

	return win;
}

XMLNode&
RecorderUI::get_state ()
{
	XMLNode* node = new XMLNode (X_("Recorder"));
	node->add_child_nocopy (Tabbable::get_state());
	return *node;
}

int
RecorderUI::set_state (const XMLNode& node, int version)
{
	return Tabbable::set_state (node, version);
}

void
RecorderUI::load_bindings ()
{
	bindings = Bindings::get_bindings (X_("Recorder"));
}

void
RecorderUI::register_actions ()
{
	Glib::RefPtr<ActionGroup> group = ActionManager::create_action_group (bindings, X_("Recorder"));
}

void
RecorderUI::set_session (Session* s)
{
	SessionHandlePtr::set_session (s);

	if (!_session) {
		return;
	}

	XMLNode* node = ARDOUR_UI::instance()->recorder_settings();
	set_state (*node, Stateful::loading_state_version);

	_session->DirtyChanged.connect (_session_connections, invalidator (*this), boost::bind (&RecorderUI::update_title, this), gui_context ());
	_session->StateSaved.connect (_session_connections, invalidator (*this), boost::bind (&RecorderUI::update_title, this), gui_context ());

	_session->RouteAdded.connect (_session_connections, invalidator (*this), boost::bind (&RecorderUI::add_routes, this, _1), gui_context ());
	TrackRecordControlBox::CatchDeletion.connect (*this, invalidator (*this), boost::bind (&RecorderUI::remove_route, this, _1), gui_context ());

	_session->config.ParameterChanged.connect (_session_connections, invalidator (*this), boost::bind (&RecorderUI::parameter_changed, this, _1), gui_context ());
	Config->ParameterChanged.connect (*this, invalidator (*this), boost::bind (&RecorderUI::parameter_changed, this, _1), gui_context ());

	update_title ();
	initial_track_display ();
	start_updating ();
}

void
RecorderUI::session_going_away ()
{
	ENSURE_GUI_THREAD (*this, &RecorderUI::session_going_away);
	SessionHandlePtr::session_going_away ();
	update_title ();
}

void
RecorderUI::update_title ()
{
	if (!own_window ()) {
		return;
	}

	if (_session) {
		string n;

		if (_session->snap_name () != _session->name ()) {
			n = _session->snap_name ();
		} else {
			n = _session->name ();
		}

		if (_session->dirty ()) {
			n = "*" + n;
		}

		WindowTitle title (n);
		title += S_("Window|Recorder");
		title += Glib::get_application_name ();
		own_window ()->set_title (title.get_string ());

	} else {
		WindowTitle title (S_("Window|Recorder"));
		title += Glib::get_application_name ();
		own_window ()->set_title (title.get_string ());
	}
}

void
RecorderUI::parameter_changed (string const& p)
{
}

void
RecorderUI::start_updating ()
{
	if (_input_ports.size ()) {
		stop_updating ();
	}

	/* Audio */
	PortManager::AudioPortMeters const& dpm (AudioEngine::instance ()->input_meters ());
#ifndef NDEBUG
	PortManager::AudioPortScopes const& ps (AudioEngine::instance ()->input_scopes ());
	assert (ps.size () == dpm.size ());
#endif

	for (PortManager::AudioPortMeters::const_iterator i = dpm.begin (); i != dpm.end (); ++i) {
		_input_ports[i->first] = boost::shared_ptr<RecorderUI::InputPort> (new InputPort (i->first, DataType::AUDIO, this));
		_input_ports[i->first]->show ();
		_meter_area.pack_start (*_input_ports[i->first], false, false);
		set_connection_count (i->first);
	}

	/* MIDI */
	PortManager::MIDIPortMeters const& mpm (AudioEngine::instance ()->midi_meters ());
#ifndef NDEBUG
	PortManager::MIDIPortMonitors const& mm (AudioEngine::instance ()->midi_monitors ());
	assert (mm.size () == mpm.size ());
#endif
	for (PortManager::MIDIPortMeters::const_iterator i = mpm.begin (); i != mpm.end (); ++i) {
		_input_ports[i->first] = boost::shared_ptr<RecorderUI::InputPort> (new InputPort (i->first, DataType::MIDI, this));
		_input_ports[i->first]->show ();
		_meter_area.pack_start (*_input_ports[i->first], false, false);
		set_connection_count (i->first);
	}

	_fast_screen_update_connection = Timers::super_rapid_connect (sigc::mem_fun (*this, &RecorderUI::update_meters));
}

void
RecorderUI::stop_updating ()
{
	_fast_screen_update_connection.disconnect ();
	container_clear (_meter_area);
	_input_ports.clear ();
}

void
RecorderUI::update_meters ()
{
	PortManager::AudioPortScopes const& ps (AudioEngine::instance ()->input_scopes ());

	for (PortManager::AudioPortScopes::const_iterator i = ps.begin (); i != ps.end (); ++i) {
		_input_ports[i->first]->update (*(i->second));
	}

	if (!contents ().is_mapped ()) {
		return;
	}

	PortManager::AudioPortMeters const& dpm (AudioEngine::instance ()->input_meters ());
	for (PortManager::AudioPortMeters::const_iterator i = dpm.begin (); i != dpm.end (); ++i) {
		_input_ports[i->first]->update (accurate_coefficient_to_dB (i->second.level), accurate_coefficient_to_dB (i->second.peak));
	}

	PortManager::MIDIPortMeters const& mpm (AudioEngine::instance ()->midi_meters ());
	for (PortManager::MIDIPortMeters::const_iterator i = mpm.begin (); i != mpm.end (); ++i) {
		_input_ports[i->first]->update ((float const*)i->second.chn_active);
	}

	PortManager::MIDIPortMonitors const& mm (AudioEngine::instance ()->midi_monitors ());
	for (PortManager::MIDIPortMonitors::const_iterator i = mm.begin (); i != mm.end (); ++i) {
		_input_ports[i->first]->update (*(i->second));
	}

	for (std::list<TrackRecordControlBox*>::const_iterator i = _recorders.begin (); i != _recorders.end (); ++i) {
		(*i)->fast_update ();
	}
}

void
RecorderUI::port_connected_or_disconnected (std::string p1, std::string p2)
{
	if (_input_ports.find (p1) != _input_ports.end ()) {
		set_connection_count (p1);
	}
	if (_input_ports.find (p2) != _input_ports.end ()) {
		set_connection_count (p2);
	}
}

void
RecorderUI::set_connection_count (std::string const& p)
{
	if (!_session) {
		return;
	}

	size_t cnt = 0;

	boost::shared_ptr<RouteList> rl = _session->get_tracks ();
	for (RouteList::const_iterator r = rl->begin(); r != rl->end(); ++r) {
		if ((*r)->input()->connected_to (p)) {
			++cnt;
		}
	}

	_input_ports[p]->set_cnt (cnt);

	// TODO: think.
	// only clear when port is spilled and cnt == 0 ?
	// otherwise only update spilled tracks if port is spilled?
	if (!_spill_port_names.empty ()) {
		for (std::map<std::string, boost::shared_ptr<InputPort> >::iterator i = _input_ports.begin (); i != _input_ports.end (); ++i) {
			i->second->spill (false);
		}
		_spill_port_names.clear ();
		update_rec_table_layout ();
	}
}

void
RecorderUI::spill_port (std::string const& p)
{
	bool ok = false;
	if (_input_ports[p]->spilled ()) {
		ok = _input_ports[p]->spill (true);
		if (!ok) {
			// TODO context-menu.. create and connect track
			return;
		}
	}

	bool update;
	if (ok) {
		std::pair<std::set<std::string>::iterator, bool> rv = _spill_port_names.insert (p);
		update = rv.second;
	} else {
		update = 0 != _spill_port_names.erase (p);
	}
	if (update) {
		update_rec_table_layout ();
	}
}

void
RecorderUI::initial_track_display ()
{
	boost::shared_ptr<RouteList> r = _session->get_tracks ();
	RouteList                    rl (*r);
	_recorders.clear ();
	add_routes (rl);
}

void
RecorderUI::add_routes (RouteList& rl)
{
	rl.sort (Stripable::Sorter ());
	for (RouteList::iterator r = rl.begin (); r != rl.end (); ++r) {
		/* we're only interested in Tracks */
		if (!boost::dynamic_pointer_cast<Track> (*r)) {
			continue;
		}

		TrackRecordControlBox* rec = new TrackRecordControlBox (/**this,*/ _session, *r);
		_recorders.push_back (rec);
	}
	update_rec_table_layout ();
}

void
RecorderUI::remove_route (TrackRecordControlBox* ra)
{
	if (!_session || _session->deletion_in_progress ()) {
		_recorders.clear ();
		return;
	}
	printf ("Remove TRACK from recarearecarea %s\n", ra->route ()->name ().c_str ());
	std::list<TrackRecordControlBox*>::const_iterator i = std::find (_recorders.begin (), _recorders.end (), ra);
	assert (i != _recorders.end ());
	_recorders.erase (i);
	update_rec_table_layout ();
}

struct TrackRecordControlBoxSorter {
	bool operator() (const TrackRecordControlBox* ca, const TrackRecordControlBox* cb)
	{
		boost::shared_ptr<ARDOUR::Stripable> const& a = ca->stripable ();
		boost::shared_ptr<ARDOUR::Stripable> const& b = cb->stripable ();
		return ARDOUR::Stripable::Sorter(true)(a, b);
	}
};

void
RecorderUI::presentation_info_changed (PBD::PropertyChange const& what_changed)
{
	if (what_changed.contains (Properties::hidden)) {
		update_rec_table_layout ();
	} else if (what_changed.contains (Properties::order)) {
		/* test if effective order changed. When deleting tracks
		 * the PI:order_key changes, but the layout does not change.
		 */
		std::list<TrackRecordControlBox*> rec (_recorders);
		_recorders.sort (TrackRecordControlBoxSorter ());
		if (_recorders != rec) {
			update_rec_table_layout ();
		}
	}
}

void
RecorderUI::update_rec_table_layout ()
{
	container_clear (_rec_table);

	bool resize = false;
	int N_COL = -1;
	int col   = 0;
	int row   = 0;

	_recorders.sort (TrackRecordControlBoxSorter ());

	std::list<TrackRecordControlBox*>::const_iterator i;
	for (i = _recorders.begin (); i != _recorders.end (); ++i) {
		if ((*i)->route ()->presentation_info ().hidden ()) {
			continue;
		}

		/* spill */
		if (!_spill_port_names.empty ()) {
			bool connected = false;
			for (std::set<std::string>::const_iterator j = _spill_port_names.begin(); j != _spill_port_names.end(); ++j) {
				if ((*i)->route ()->input()->connected_to (*j)) {
					connected = true;
					break;
				}
			}
			if (!connected) {
				continue;
			}
		}

		_rec_table.attach (**i, col, col + 1, row, row + 1, Gtk::SHRINK, Gtk::SHRINK, 2, 4);
		(*i)->show ();

		if (N_COL < 0) {
			Gtk::Requisition r = (*i)->size_request ();
			if (_rec_box_width != r.width + 4) {
				resize = true;
			}
			_rec_box_width = r.width + 4;
			N_COL = _rec_area.get_width () / _rec_box_width;
		}

		if (++col >= N_COL) {
			col = 0;
			++row;
		}
	}

	printf ("RecorderUI::update_rec_table_layout %d %d\n", N_COL, resize);

	if (N_COL > 0) {
		_rec_area_cols = N_COL;
	}
	if (resize) {
		_rec_area.queue_resize ();
	}
}

void
RecorderUI::rec_area_size_allocate (Gtk::Allocation& allocation)
{
	if (_rec_area_cols == allocation.get_width () / _rec_box_width) {
		return;
	}
	update_rec_table_layout ();
}

void
RecorderUI::rec_area_size_request (GtkRequisition* requisition)
{
	Gtk::Requisition r  = _rec_table.size_request ();
	requisition->width  = _rec_box_width * 2;
	requisition->height = r.height;
}

/* ****************************************************************************/
#define PX_SCALE(px) std::max ((float)px, rintf ((float)px* UIConfiguration::instance ().get_ui_scale ()))

RecorderUI::InputPort::InputPort (std::string const& name, ARDOUR::DataType dt, RecorderUI* parent)
	: _dt (dt)
	, _spill_btn ("0", ArdourButton::led_default_elements, true)
	, _name_label (name, (ArdourButton::Element)(ArdourButton::Edge | ArdourButton::Body | ArdourButton::Text | ArdourButton::Inactive))
	, _audio_meter (0)
	, _audio_scope (0)
	, _midi_meter (0)
	, _midi_monitor (0)
{

	_spill_btn.set_name ("generic button");
	_spill_btn.set_can_focus (true);
	_spill_btn.set_led_left (true);
	_spill_btn.show ();
	_spill_btn.signal_clicked.connect (sigc::bind (sigc::mem_fun (*parent, &RecorderUI::spill_port), name));

	_name_label.set_corner_radius (2);
	_name_label.set_name ("meterbridge label");

	int nh = 120 * UIConfiguration::instance ().get_ui_scale ();
	_name_label.set_text_ellipsize (Pango::ELLIPSIZE_MIDDLE);
	_name_label.set_layout_ellipsize_width (nh * PANGO_SCALE);
	_name_label.set_size_request (nh, PX_SCALE (18));
	_name_label.show ();

	set_tooltip (_name_label, Gtkmm2ext::markup_escape_text (name));

	_box.pack_start (_spill_btn, false, false);
	_box.pack_start (_name_label, false, false);

	if (_dt == DataType::AUDIO) {
		_audio_meter = new FastMeter (
		    (uint32_t)floor (UIConfiguration::instance ().get_meter_hold ()),
		    18, FastMeter::Horizontal, PX_SCALE (240),
		    UIConfiguration::instance ().color ("meter color0"),
		    UIConfiguration::instance ().color ("meter color1"),
		    UIConfiguration::instance ().color ("meter color2"),
		    UIConfiguration::instance ().color ("meter color3"),
		    UIConfiguration::instance ().color ("meter color4"),
		    UIConfiguration::instance ().color ("meter color5"),
		    UIConfiguration::instance ().color ("meter color6"),
		    UIConfiguration::instance ().color ("meter color7"),
		    UIConfiguration::instance ().color ("meter color8"),
		    UIConfiguration::instance ().color ("meter color9"),
		    UIConfiguration::instance ().color ("meter background bottom"),
		    UIConfiguration::instance ().color ("meter background top"),
		    0x991122ff, // red highlight gradient Bot
		    0x551111ff, // red highlight gradient Top
		    (115.0 * log_meter0dB (-18)),
		    89.125,  // 115.0 * log_meter0dB(-9);
		    106.375, // 115.0 * log_meter0dB(-3);
		    115.0,   // 115.0 * log_meter0dB(0);
		    (UIConfiguration::instance ().get_meter_style_led () ? 3 : 1));

		_audio_scope = new InputScope (parent->session ()->nominal_sample_rate ());

		_audio_meter->show ();
		_audio_scope->show ();
		_box.pack_start (*_audio_meter, false, false);
		_box.pack_start (*_audio_scope, false, false);

	} else if (_dt == DataType::MIDI) {
		_midi_meter = new EventMeter ();
		_midi_monitor = new EventMonitor ();
		_midi_meter->show ();
		_midi_monitor->show ();
		_box.pack_start (*_midi_meter, false, false);
		_box.pack_start (*_midi_monitor, false, false);
	}

	pack_start (_box, true, false);
	_box.show ();
}

RecorderUI::InputPort::~InputPort ()
{
	delete _audio_meter;
	delete _audio_scope;
	delete _midi_meter;
	delete _midi_monitor;
}

void
RecorderUI::InputPort::update (float l, float p)
{
	assert (_dt == DataType::AUDIO && _audio_meter);
	_audio_meter->set (log_meter0dB (l), log_meter0dB (p));
}

void
RecorderUI::InputPort::update (ARDOUR::CircularSampleBuffer& csb)
{
	assert (_dt == DataType::AUDIO && _audio_scope);
	_audio_scope->update (csb);
}

void
RecorderUI::InputPort::update (float const* v)
{
	assert (_dt == DataType::MIDI && _midi_meter);
	_midi_meter->update (v);
}

void
RecorderUI::InputPort::update (ARDOUR::CircularEventBuffer& ceb)
{
	assert (_dt == DataType::MIDI && _midi_monitor);
	_midi_monitor->update (ceb);
}

void
RecorderUI::InputPort::set_cnt (size_t cnt)
{
	_spill_btn.set_text (PBD::to_string (cnt));
}

bool
RecorderUI::InputPort::spill (bool en)
{
	bool active = _spill_btn.get_active ();
	bool act = active;

	if (!en) {
		act = false;
	}

	if (_spill_btn.get_text () == "0") {
		act = false;
	}

	if (active != act) {
		_spill_btn.set_active (act);
	}
	return act;
}

bool
RecorderUI::InputPort::spilled () const
{
	return _spill_btn.get_active ();
}

/* ****************************************************************************/

RecorderUI::InputScope::InputScope (samplecnt_t rate, int w, int h)
	: _xpos (0)
	, _rate (rate)
{
	_surface = Cairo::ImageSurface::create (Cairo::FORMAT_ARGB32, w, h);
}

RecorderUI::InputScope::~InputScope ()
{
}

void
RecorderUI::InputScope::clear ()
{
}

void
RecorderUI::InputScope::update (CircularSampleBuffer& csb)
{
	int w = _surface->get_width();
	int h = _surface->get_height();
	double h_2 = h / 2.0;

	// TODO: cache, subscribe to signal
	bool show_clip = UIConfiguration::instance().get_show_waveform_clipping();
	const float clip_level = dB_to_coefficient (UIConfiguration::instance().get_waveform_clip_level ());
	bool logscale = UIConfiguration::instance().get_waveform_scale() == Logarithmic;

	int spp = 5.0 /*sec*/ * _rate / w; // samples / pixel

	Cairo::RefPtr<Cairo::Context> cr;
	bool have_data = false;
	float minf, maxf;

	while (csb.read (minf, maxf, spp)) {
		if (!have_data) {
			cr = Cairo::Context::create (_surface);
			have_data = true;
		}
		/* see also ExportReport::draw_waveform */
		cr->rectangle (_xpos, 0, 1, h);
		cr->set_operator (Cairo::OPERATOR_SOURCE);
		cr->set_source_rgba (0, 0, 0, 0);
		cr->fill ();

		cr->set_operator (Cairo::OPERATOR_OVER);
		cr->set_line_width (1.0);

		if (show_clip && (maxf >= clip_level || -minf >= clip_level)) {
			cr->set_source_rgba (.9, .3, .3, 1.0);
		} else {
			cr->set_source_rgba (.7, .7, .7, 1.0);
		}

		if (logscale) {
			if (maxf > 0) {
				maxf =  alt_log_meter (fast_coefficient_to_dB (maxf));
			} else {
				maxf = -alt_log_meter (fast_coefficient_to_dB (-maxf));
			}
			if (minf > 0) {
				minf =  alt_log_meter (fast_coefficient_to_dB (minf));
			} else {
				minf = -alt_log_meter (fast_coefficient_to_dB (-minf));
			}
		}

		cr->move_to (_xpos + .5, h_2 - h_2 * maxf);
		cr->line_to (_xpos + .5, h_2 - h_2 * minf);
		cr->stroke ();

		if (++_xpos >= w) {
			_xpos = 0;
		}
	}

	if (have_data) {
		_surface->flush ();
		queue_draw ();
	}
}

void
RecorderUI::InputScope::render (Cairo::RefPtr<Cairo::Context> const& cr, cairo_rectangle_t* r)
{
	cr->rectangle (r->x, r->y, r->width, r->height);
	cr->clip ();

	int w = _surface->get_width();
	int h = _surface->get_height();

	cr->set_operator (Cairo::OPERATOR_OVER);
	cr->set_source (_surface, 0.0 - _xpos, 0);
	cr->paint ();

	cr->set_source (_surface, w - _xpos, 0);
	cr->paint ();

	/* zero line */
	double h_2 = h / 2.0;
	cr->set_line_width (1.0);
	cr->set_source_rgba (.3, .3, .3, 0.7);
	cr->move_to (0, h_2);
	cr->line_to (w, h_2);
	cr->stroke ();
}

void
RecorderUI::InputScope::on_size_request (Gtk::Requisition* req)
{
	req->width = _surface->get_width();
	req->height = _surface->get_height();
}

void
RecorderUI::InputScope::on_size_allocate (Gtk::Allocation& a)
{
	CairoWidget::on_size_allocate (a);
}

/* ****************************************************************************/

RecorderUI::EventMeter::EventMeter ()
{
	_layout = Pango::Layout::create (get_pango_context());
	_layout->set_font_description (UIConfiguration::instance().get_SmallMonospaceFont());
}

RecorderUI::EventMeter::~EventMeter ()
{
}

void
RecorderUI::EventMeter::update (float const* v)
{
	memcpy (_chn, v, sizeof (_chn));
	queue_draw ();
}

void
RecorderUI::EventMeter::render (Cairo::RefPtr<Cairo::Context> const& cr, cairo_rectangle_t* r)
{
	cr->rectangle (r->x, r->y, r->width, r->height);
	cr->clip ();

	float y0 = 2.5; // XXX depending on fontsize
	int ww = 13;
	int hh = 20;

	cr->set_operator (Cairo::OPERATOR_OVER);
	for (size_t i = 0; i < 17; ++i) {
		float x0 = 1.5 + 15 * i;
		cr->set_line_width (1.0);
		cr->rectangle (x0, y0, ww, hh);
		cr->set_source_rgba (_chn[i] / 1.2 , 0.1 + _chn[i] / 1.5, 0.1 + _chn[i] / 1.75, 1.0);
		cr->fill_preserve ();
		cr->set_source_rgba (1, 1, 1, 1.0);
		cr->stroke ();

		cr->save ();
		int w, h;
		if (i < 16) {
			_layout->set_text ("C" + PBD::to_string (i + 1));
		} else {
			_layout->set_text ("SyS");
		}
		_layout->get_pixel_size (h, w);
		cr->move_to (x0 + .5 * (ww - w), y0 + .5 * (hh + h));
		cr->rotate (M_PI / -2.0);
		_layout->show_in_cairo_context (cr);
		cr->restore ();
	}
}

void
RecorderUI::EventMeter::on_size_request (Gtk::Requisition* req)
{
	req->width = 17 * 15;
	req->height = 24;
}

void
RecorderUI::EventMeter::on_size_allocate (Gtk::Allocation& a)
{
	CairoWidget::on_size_allocate (a);
}

/* ****************************************************************************/

RecorderUI::EventMonitor::EventMonitor ()
{
	_layout = Pango::Layout::create (get_pango_context());
	_layout->set_font_description (UIConfiguration::instance().get_SmallMonospaceFont());
}

RecorderUI::EventMonitor::~EventMonitor ()
{
}

void
RecorderUI::EventMonitor::update (CircularEventBuffer& ceb)
{
	if (ceb.read (_l)) {
		queue_draw ();
	}
}

void
RecorderUI::EventMonitor::render (Cairo::RefPtr<Cairo::Context> const& cr, cairo_rectangle_t* r)
{
	int ww = 240;
	int hh = 24;

	cr->set_source_rgba (.7, .7, .7, 1.0);
	for (CircularEventBuffer::EventList::const_iterator i = _l.begin (); i != _l.end(); ++i) {
		if (i->data[0] == 0) {
			break;
		}
		int chn = 1 + (i->data[0] & 0x0f);
		char tmp[32];
		switch (i->data[0] & 0xf0) {
			case MIDI_CMD_NOTE_OFF:
				sprintf (tmp, "%02d\u2669Off\n %4s ", chn, ParameterDescriptor::midi_note_name (i->data[1]).c_str());
				break;
			case MIDI_CMD_NOTE_ON:
				sprintf (tmp, "%02d\u2669 On\n %4s ", chn, ParameterDescriptor::midi_note_name (i->data[1]).c_str());
				break;
			case MIDI_CMD_NOTE_PRESSURE:
				sprintf (tmp, "%02d\u2669 KP\n %4s ", chn, ParameterDescriptor::midi_note_name (i->data[1]).c_str());
				break;
			case MIDI_CMD_CONTROL:
				sprintf (tmp, "%02d CC \n%02x  %02x", chn, i->data[1], i->data[2]);
				break;
			case MIDI_CMD_PGM_CHANGE:
				sprintf (tmp, "%02d PC \n  %02x  ", chn, i->data[1]);
				break;
			case MIDI_CMD_CHANNEL_PRESSURE:
				sprintf (tmp, "%02d KP \n  %02x  ", chn, i->data[1]);
				break;
			case MIDI_CMD_BENDER:
				sprintf (tmp, "%02d PB \n %04x ", chn, i->data[2] |  i->data[3] << 7);
				break;
			case MIDI_CMD_COMMON_SYSEX:
				// TODO sub-type
				sprintf (tmp, "Sys.Ex");
				break;
		}

		int w, h;
		_layout->set_text (tmp);
		_layout->get_pixel_size (w, h);
		cr->move_to (ww - w, .5 * (hh - h));
		_layout->show_in_cairo_context (cr);
		ww -= w + 12;
		if (ww < 0) {
			break;
		}
	}
}

void
RecorderUI::EventMonitor::on_size_request (Gtk::Requisition* req)
{
	req->width = 240;
	req->height = 24;
}

void
RecorderUI::EventMonitor::on_size_allocate (Gtk::Allocation& a)
{
	CairoWidget::on_size_allocate (a);
}
