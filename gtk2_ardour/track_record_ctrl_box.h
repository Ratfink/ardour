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

#ifndef __gtkardour_track_record_ctrl_box_h_
#define __gtkardour_track_record_ctrl_box_h_

#include <cmath>
#include <vector>

#include <gtkmm/alignment.h>
#include <gtkmm/box.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/eventbox.h>
#include <gtkmm/frame.h>
#include <gtkmm/separator.h>
#include <gtkmm/sizegroup.h>

#include "pbd/stateful.h"

#include "ardour/ardour.h"
#include "ardour/types.h"

#include "widgets/ardour_button.h"

#include "level_meter.h"
#include "route_ui.h"

namespace ARDOUR
{
	class Route;
	class RouteGroup;
	class Session;
}

class LevelMeterHBox;

class TrackRecordControlBox : public Gtk::VBox, public AxisView, public RouteUI
{
public:
	TrackRecordControlBox (ARDOUR::Session*, boost::shared_ptr<ARDOUR::Route>);
	~TrackRecordControlBox ();

	/* AxisView */
	std::string name () const;
	Gdk::Color  color () const;

	boost::shared_ptr<ARDOUR::Stripable> stripable() const {
		return RouteUI::stripable();
	}

	void set_session (ARDOUR::Session* s);

	void fast_update ();

	static PBD::Signal1<void, TrackRecordControlBox*> CatchDeletion;

protected:
	void self_delete ();

	void on_size_allocate (Gtk::Allocation&);
	void on_size_request (Gtk::Requisition*);

	/* AxisView */
	std::string state_id () const;

	/* route UI */
	void set_button_names ();
	void blink_rec_display (bool onoff);
	void route_active_changed ();

private:
	void on_theme_changed ();
	void parameter_changed (std::string const& p);

	void set_name_label ();

	void reset_peak_display ();
	void reset_route_peak_display (ARDOUR::Route*);
	void reset_group_peak_display (ARDOUR::RouteGroup*);

	/* RouteUI */
	void route_property_changed (const PBD::PropertyChange&);
	void route_color_changed ();
	void update_sensitivity ();

	bool _clear_meters;

	Gtk::Frame _frame;
	Gtk::Table _ctrls;

	LevelMeterHBox*             _level_meter;
	ArdourWidgets::ArdourButton _number_label;

	Glib::RefPtr<Gtk::SizeGroup>        _ctrls_button_size_group;
	static Glib::RefPtr<Gtk::SizeGroup> _track_number_size_group;
	static Glib::RefPtr<Gtk::SizeGroup> _level_meters_size_group;

	PBD::ScopedConnectionList _route_connections;
};

#endif
