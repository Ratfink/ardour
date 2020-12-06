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

#ifndef __gtk_ardour_recorder_ui_h__
#define __gtk_ardour_recorder_ui_h__

#include <boost/shared_ptr.hpp>
#include <list>
#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/scrolledwindow.h>

#include "ardour/session_handle.h"
#include "ardour/circular_buffer.h"

#include "gtkmm2ext/bindings.h"
#include "gtkmm2ext/cairo_widget.h"

#include "widgets/ardour_button.h"
#include "widgets/pane.h"
#include "widgets/tabbable.h"

namespace ArdourWidgets
{
	class FastMeter;
}

class TrackRecordControlBox;

class RecorderUI : public ArdourWidgets::Tabbable, public ARDOUR::SessionHandlePtr, public PBD::ScopedConnectionList
{
public:
	RecorderUI ();
	~RecorderUI ();

	void set_session (ARDOUR::Session*);
	void cleanup ();

	XMLNode& get_state ();
	int set_state (const XMLNode&, int /* version */);

	Gtk::Window* use_own_window (bool and_fill_it);

	void spill_port (std::string const&);

private:
	void load_bindings ();
	void register_actions ();
	void update_title ();
	void session_going_away ();
	void parameter_changed (std::string const&);
	void presentation_info_changed (PBD::PropertyChange const&);

	void start_updating ();
	void stop_updating ();
	void update_meters ();

	void initial_track_display ();
	void add_routes (ARDOUR::RouteList&);
	void remove_route (TrackRecordControlBox*);
	void update_rec_table_layout ();
	void rec_area_size_request (GtkRequisition*);
	void rec_area_size_allocate (Gtk::Allocation&);

	void set_connection_count (std::string const&);
	void port_connected_or_disconnected (std::string, std::string);

	Gtkmm2ext::Bindings*  bindings;
	Gtk::VBox            _content;
	ArdourWidgets::VPane _pane;
	Gtk::ScrolledWindow  _rec_scroller;
	Gtk::VBox            _rec_area;
	Gtk::Table           _rec_table;
	Gtk::ScrolledWindow  _meter_scroller;
	Gtk::VBox            _meter_area;

	int  _rec_box_width;
	int  _rec_area_cols;

	std::set<std::string> _spill_port_names;

	sigc::connection          _fast_screen_update_connection;
	PBD::ScopedConnectionList _engine_connections;

	class InputScope : public CairoWidget
	{
		public:
			InputScope (ARDOUR::samplecnt_t, int w = 180, int h = 25);
			~InputScope ();

			void clear ();
			void update (ARDOUR::CircularSampleBuffer&);

		protected:
			void render (Cairo::RefPtr<Cairo::Context> const&, cairo_rectangle_t*);
			void on_size_request (Gtk::Requisition*);
			void on_size_allocate (Gtk::Allocation&);

		private:
			int                 _xpos;
			ARDOUR::samplecnt_t _rate;

			Cairo::RefPtr<Cairo::ImageSurface> _surface;
	};

	class EventMeter : public CairoWidget
	{
		public:
			EventMeter ();
			~EventMeter ();
			void update (float const*);

		protected:
			void render (Cairo::RefPtr<Cairo::Context> const&, cairo_rectangle_t*);
			void on_size_request (Gtk::Requisition*);
			void on_size_allocate (Gtk::Allocation&);

		private:
			float _chn[17];
			Glib::RefPtr<Pango::Layout> _layout;
	};

	class EventMonitor : public CairoWidget
	{
		public:
			EventMonitor ();
			~EventMonitor ();
			void clear ();
			void update (ARDOUR::CircularEventBuffer&);

		protected:
			void render (Cairo::RefPtr<Cairo::Context> const&, cairo_rectangle_t*);
			void on_size_request (Gtk::Requisition*);
			void on_size_allocate (Gtk::Allocation&);

		private:
			ARDOUR::CircularEventBuffer::EventList _l;
			Glib::RefPtr<Pango::Layout> _layout;
	};

	class InputPort : public Gtk::VBox
	{
		public:
			InputPort (std::string const&, ARDOUR::DataType, RecorderUI*);
			~InputPort ();

			void set_cnt (size_t);
			bool spill (bool);
			bool spilled () const;

			void update (float, float); // FastMeter
			void update (float const*); // EventMeter
			void update (ARDOUR::CircularSampleBuffer&); // InputScope
			void update (ARDOUR::CircularEventBuffer&); // EventMonitor

		private:
			ARDOUR::DataType            _dt;
			Gtk::HBox                   _box;
			ArdourWidgets::ArdourButton _spill_btn;
			ArdourWidgets::ArdourButton _name_label;
			ArdourWidgets::FastMeter*   _audio_meter;
			InputScope*                 _audio_scope;
			EventMeter*                 _midi_meter;
			EventMonitor*               _midi_monitor;
	};

	std::map<std::string, boost::shared_ptr<InputPort> > _input_ports;
	std::list<TrackRecordControlBox*>                    _recorders;
};

#endif /* __gtk_ardour_recorder_ui_h__ */
