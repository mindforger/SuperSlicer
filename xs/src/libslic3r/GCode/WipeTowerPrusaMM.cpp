/*

TODO LIST
---------

1. cooling moves - DONE
2. priming extrusions - DONE (fixed wiping volume so far)
3. account for perimeter and finish_layer extrusions and subtract it from last wipe
4. Peter's wipe tower - layer's are not exactly square
5. Peter's wipe tower - variable width for higher levels
6. Peter's wipe tower - make sure it is not too sparse (apply max_bridge_distance and make last wipe longer)
7. Peter's wipe tower - enable enhanced first layer adhesion 

*/

#include "WipeTowerPrusaMM.hpp"

#include <assert.h>
#include <math.h>
#include <fstream>
#include <iostream>
#include <vector>

#include "Analyzer.hpp"

#if defined(__linux) || defined(__GNUC__ )
#include <strings.h>
#endif /* __linux */

#ifdef _MSC_VER 
#define strcasecmp _stricmp
#endif

constexpr bool  peters_wipe_tower = false;	// sparse wipe tower inspired by Peter's post processor - not finished yet
constexpr float min_layer_difference = 2*m_perimeter_width;
constexpr float max_bridge_distance = 10.f; // in mm
constexpr bool  improve_first_layer_adhesion = true;
// experimental: ramming speed (mm^3/s) sampled in 0.25s intervals (one filament so far)
const std::vector<float> ramming_speed = {7.6, 7.6, 7.6, 7.6, 9.0, 9.0, 9.0, 10.7, 10.7, 10.7};
constexpr float ramming_step_multiplicator = 1.2f; // extra spacing may be needed for some materials
constexpr float ramming_line_width_multiplicator = 1.5f;

// experimental: time requested for cooling in seconds (common for all materials so far)
constexpr float cooling_time = 14; // PVA: 20; SCAFF: 17; PLA+others: 14


// volumes in mm^3 required for wipe: {{from 0 to ...},{from 1 to ...},{from 2 to ...},{from 3 to ...}}, usage [from][to]
const std::vector<std::vector<float>> wipe_volumes = {{  0,120, 10, 50},
													  { 20,  0, 30, 40},
													  { 90, 20,  0, 85},
													  {100,140, 30,  0}};

/*const std::vector<std::vector<float>> wipe_volumes = {{0, 67, 67, 67},
													  {67, 0, 67, 67},
													  {67, 67, 0, 67},
													  {67, 67, 67, 0}};
*/
/*const std::vector<std::vector<float>> wipe_volumes = {{0, 10, 10, 10},
													  {10, 0, 10, 10},
													  {10, 10, 0, 10},
													  {10, 10, 10, 0}};
*/

namespace Slic3r
{

namespace PrusaMultiMaterial {

class Writer
{
public:
	Writer() : 
		m_current_pos(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
		m_current_z(0.f),
		m_current_feedrate(0.f),
		m_layer_height(0.f),
		m_extrusion_flow(0.f),
		m_preview_suppressed(false),
		m_elapsed_time(0.f) {}

	Writer& 			 set_initial_position(const WipeTower::xy &pos) { 
		m_start_pos = pos;
		m_current_pos = pos;
		return *this;
	}

	Writer&				 set_initial_tool(const unsigned int tool) { m_current_tool = tool; return *this; }

	Writer&				 set_z(float z) 
		{ m_current_z = z; return *this; }

	Writer&				 set_layer_height(float layer_height)
		{ m_layer_height = layer_height; return *this; }

	Writer& 			 set_extrusion_flow(float flow)
		{ m_extrusion_flow = flow; return *this; }
		
	Writer&				 set_rotation(WipeTower::xy& pos, float width, float depth, float angle)
		{ m_wipe_tower_pos = pos; m_wipe_tower_width = width; m_wipe_tower_depth=depth; m_angle_deg = angle; return (*this); }

	Writer&				 set_y_shift(float shift)
		{ m_y_shift = shift; return (*this); }

	// Suppress / resume G-code preview in Slic3r. Slic3r will have difficulty to differentiate the various
	// filament loading and cooling moves from normal extrusion moves. Therefore the writer
	// is asked to suppres output of some lines, which look like extrusions.
	Writer& 			 suppress_preview() { m_preview_suppressed = true; return *this; }
	Writer& 			 resume_preview() { m_preview_suppressed = false; return *this; }

	Writer& 			 feedrate(float f)
	{
		if (f != m_current_feedrate)
			m_gcode += "G1" + set_format_F(f) + "\n";
		return *this;
	}

	const std::string&   gcode() const { return m_gcode; }
	const std::vector<WipeTower::Extrusion>& extrusions() const { return m_extrusions; }
	float                x()     const { return m_current_pos.x; }
	float                y()     const { return m_current_pos.y; }
	const WipeTower::xy& pos()   const { return m_current_pos; }
	const WipeTower::xy	 start_pos_rotated() const { return m_start_pos.rotate(m_wipe_tower_pos, m_wipe_tower_width, m_wipe_tower_depth, m_angle_deg); }
	const WipeTower::xy  pos_rotated() const { return m_current_pos.rotate(m_wipe_tower_pos, m_wipe_tower_width, m_wipe_tower_depth, m_angle_deg); }
	float 				 elapsed_time() const { return m_elapsed_time; }

	// Extrude with an explicitely provided amount of extrusion.
	Writer& extrude_explicit(float x, float y, float e, float f = 0.f) 
	{
		if (x == m_current_pos.x && y == m_current_pos.y && e == 0.f && (f == 0.f || f == m_current_feedrate))
			// Neither extrusion nor a travel move.
			return *this;

		float dx = x - m_current_pos.x;
		float dy = y - m_current_pos.y;
		double len = sqrt(dx*dx+dy*dy);


		// For rotated wipe tower, transform position to printer coordinates
		WipeTower::xy rotated_current_pos(WipeTower::xy(m_current_pos,0.f,m_y_shift).rotate(m_wipe_tower_pos, m_wipe_tower_width, m_wipe_tower_depth, m_angle_deg));
		WipeTower::xy rot(WipeTower::xy(x,y+m_y_shift).rotate(m_wipe_tower_pos, m_wipe_tower_width, m_wipe_tower_depth, m_angle_deg));

		if (! m_preview_suppressed && e > 0.f && len > 0.) {
			// Width of a squished extrusion, corrected for the roundings of the squished extrusions.
			// This is left zero if it is a travel move.
			float width = float(double(e) * Filament_Area / (len * m_layer_height));
			// Correct for the roundings of a squished extrusion.
			width += m_layer_height * float(1. - M_PI / 4.);
			if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos)
				m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
			m_extrusions.emplace_back(WipeTower::Extrusion(WipeTower::xy(rot.x, rot.y), width, m_current_tool));			
		}

		m_gcode += "G1";
		if (rot.x != rotated_current_pos.x)
			m_gcode += set_format_X(rot.x);
		if (rot.y != rotated_current_pos.y)
			m_gcode += set_format_Y(rot.y);
			
		// Transform current position back to wipe tower coordinates (was updated by set_format_X)
		m_current_pos.x = x;
		m_current_pos.y = y;

		if (e != 0.f)
			m_gcode += set_format_E(e);

		if (f != 0.f && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		
		
		
		
		// Update the elapsed time with a rough estimate.
		m_elapsed_time += ((len == 0) ? std::abs(e) : len) / m_current_feedrate * 60.f;
		m_gcode += "\n";
		return *this;
	}

	Writer& extrude_explicit(const WipeTower::xy &dest, float e, float f = 0.f) 
		{ return extrude_explicit(dest.x, dest.y, e, f); }

	// Travel to a new XY position. f=0 means use the current value.
	Writer& travel(float x, float y, float f = 0.f)
		{ return extrude_explicit(x, y, 0.f, f); }

	Writer& travel(const WipeTower::xy &dest, float f = 0.f) 
		{ return extrude_explicit(dest.x, dest.y, 0.f, f); }

	// Extrude a line from current position to x, y with the extrusion amount given by m_extrusion_flow.
	Writer& extrude(float x, float y, float f = 0.f)
	{
		float dx = x - m_current_pos.x;
		float dy = y - m_current_pos.y;
		return extrude_explicit(x, y, sqrt(dx*dx+dy*dy) * m_extrusion_flow, f);
	}

	Writer& extrude(const WipeTower::xy &dest, const float f = 0.f) 
		{ return extrude(dest.x, dest.y, f); }

	Writer& load(float e, float f = 0.f)
	{
		if (e == 0.f && (f == 0.f || f == m_current_feedrate))
			return *this;
		m_gcode += "G1";
		if (e != 0.f)
			m_gcode += set_format_E(e);
		if (f != 0.f && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}
 
	// Derectract while moving in the X direction.
	// If |x| > 0, the feed rate relates to the x distance,
	// otherwise the feed rate relates to the e distance.
	Writer& load_move_x(float x, float e, float f = 0.f)
		{ return extrude_explicit(x, m_current_pos.y, e, f); }

	Writer& retract(float e, float f = 0.f)
		{ return load(-e, f); }

	// Elevate the extruder head above the current print_z position.
	Writer& z_hop(float hop, float f = 0.f)
	{ 
		m_gcode += std::string("G1") + set_format_Z(m_current_z + hop);
		if (f != 0 && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	// Lower the extruder head back to the current print_z position.
	Writer& z_hop_reset(float f = 0.f) 
		{ return z_hop(0, f); }

	// Move to x1, +y_increment,
	// extrude quickly amount e to x2 with feed f.
	Writer& ram(float x1, float x2, float dy, float e0, float e, float f)
	{
		extrude_explicit(x1, m_current_pos.y + dy, e0, f);
		extrude_explicit(x2, m_current_pos.y, e);
		return *this;
	}

	// Let the end of the pulled out filament cool down in the cooling tube
	// by moving up and down and moving the print head left / right
	// at the current Y position to spread the leaking material.
	Writer& cool(float x1, float x2, float e1, float e2, float f)
	{
		extrude_explicit(x1, m_current_pos.y, e1, f);
		extrude_explicit(x2, m_current_pos.y, e2);
		return *this;
	}

	Writer& set_tool(int tool) 
	{
		char buf[64];
		sprintf(buf, "T%d\n", tool);
		m_gcode += buf;
		m_current_tool = tool;
		return *this;
	}

	// Set extruder temperature, don't wait by default.
	Writer& set_extruder_temp(int temperature, bool wait = false)
	{
		char buf[128];
		sprintf(buf, "M%d S%d\n", wait ? 109 : 104, temperature);
		m_gcode += buf;
		return *this;
	};

	// Set speed factor override percentage.
	Writer& speed_override(int speed) 
	{
		char buf[128];
		sprintf(buf, "M220 S%d\n", speed);
		m_gcode += buf;
		return *this;
	};

	// Set digital trimpot motor
	Writer& set_extruder_trimpot(int current) 
	{
		char buf[128];
		sprintf(buf, "M907 E%d\n", current);
		m_gcode += buf;
		return *this;
	};

	Writer& flush_planner_queue() 
	{ 
		m_gcode += "G4 S0\n"; 
		return *this;
	}

	// Reset internal extruder counter.
	Writer& reset_extruder()
	{ 
		m_gcode += "G92 E0\n";
		return *this;
	}

	Writer& comment_with_value(const char *comment, int value)
	{
		char strvalue[64];
		sprintf(strvalue, "%d", value);
		m_gcode += std::string(";") + comment + strvalue + "\n";
		return *this;
	};


	Writer& set_fan(unsigned int speed)
	{
		if (speed == m_last_fan_speed)
			return *this;
				
		if (speed == 0)
			m_gcode += "M107\n";
		else
		{
			m_gcode += "M106 S";
			char buf[128];
			sprintf(buf,"%u\n",(unsigned int)(255.0 * speed / 100.0));
			m_gcode += buf;
		}
		m_last_fan_speed = speed;
		return *this;
	}

	Writer& comment_material(WipeTowerPrusaMM::material_type material)
	{
		m_gcode += "; material : ";
		switch (material)
		{
		case WipeTowerPrusaMM::PVA:
			m_gcode += "#8 (PVA)";
			break;
		case WipeTowerPrusaMM::SCAFF:
			m_gcode += "#5 (Scaffold)";
			break;
		case WipeTowerPrusaMM::FLEX:
			m_gcode += "#4 (Flex)";
			break;
		default:
			m_gcode += "DEFAULT (PLA)";
			break;
		}
		m_gcode += "\n";
		return *this;
	};

	Writer& append(const char *text) { m_gcode += text; return *this; }

private:
	WipeTower::xy m_start_pos;
	WipeTower::xy m_current_pos;
	float    	  m_current_z;
	float 	  	  m_current_feedrate;
	unsigned int  m_current_tool;
	float 		  m_layer_height;
	float 	  	  m_extrusion_flow;
	bool		  m_preview_suppressed;
	std::string   m_gcode;
	std::vector<WipeTower::Extrusion> m_extrusions;
	float         m_elapsed_time;
	float   	  m_angle_deg = 0.f;
	float		  m_y_shift = 0.f;
	WipeTower::xy m_wipe_tower_pos;
	float 		  m_wipe_tower_width = 0.f;
	float		  m_wipe_tower_depth = 0.f;
	float		  m_last_fan_speed = 0.f;

		std::string
		set_format_X(float x)
	{
		char buf[64];
		sprintf(buf, " X%.3f", x);
		m_current_pos.x = x;
		return buf;
	}

	std::string   set_format_Y(float y) {
		char buf[64];
		sprintf(buf, " Y%.3f", y);
		m_current_pos.y = y;
		return buf;
	}

	std::string   set_format_Z(float z) {
		char buf[64];
		sprintf(buf, " Z%.3f", z);
		return buf;
	}

	std::string   set_format_E(float e) {
		char buf[64];
		sprintf(buf, " E%.4f", e);
		return buf;
	}

	std::string   set_format_F(float f) {
		char buf[64];
		sprintf(buf, " F%d", int(floor(f + 0.5f)));
		m_current_feedrate = f;
		return buf;
	}

	Writer& operator=(const Writer &rhs);
};

/*
class Material
{
public:
	std::string 				name;
	std::string 				type;

	struct RammingStep {
//		float length;
		float extrusion_multiplier; // sirka linky
		float extrusion;
		float speed;
	};
	std::vector<RammingStep> 			ramming_sequence;

	// Number and speed of the cooling moves.
	std::vector<float>					cooling_moves;

	// Percentage of the speed overide, in pairs of <z, percentage>
	std::vector<std::pair<float, int>> 	speed_override;
};
*/

}; // namespace PrusaMultiMaterial

WipeTowerPrusaMM::material_type WipeTowerPrusaMM::parse_material(const char *name)
{
	if (strcasecmp(name, "PLA") == 0)
		return PLA;
	if (strcasecmp(name, "ABS") == 0)
		return ABS;
	if (strcasecmp(name, "PET") == 0)
		return PET;
	if (strcasecmp(name, "HIPS") == 0)
		return HIPS;
	if (strcasecmp(name, "FLEX") == 0)
		return FLEX;
	if (strcasecmp(name, "SCAFF") == 0)
		return SCAFF;
	if (strcasecmp(name, "EDGE") == 0)
		return EDGE;
	if (strcasecmp(name, "NGEN") == 0)
		return NGEN;
	if (strcasecmp(name, "PVA") == 0)
		return PVA;
	return INVALID;
}

// Returns gcode to prime the nozzles at the front edge of the print bed.
WipeTower::ToolChangeResult WipeTowerPrusaMM::prime(
	// print_z of the first layer.
	float 						first_layer_height, 
	// Extruder indices, in the order to be primed. The last extruder will later print the wipe tower brim, print brim and the object.
	const std::vector<unsigned int> &tools,
	// If true, the last priming are will be the same as the other priming areas, and the rest of the wipe will be performed inside the wipe tower.
	// If false, the last priming are will be large enough to wipe the last extruder sufficiently.
	bool 						last_wipe_inside_wipe_tower, 
	// May be used by a stand alone post processor.
	Purpose 					purpose)
{
	this->set_layer(first_layer_height, first_layer_height, tools.size(), true, false);

	// Calculate the amount of wipe over the wipe tower brim following the prime, decrease wipe_area
	// with the amount of material extruded over the brim.
	// DOESN'T MAKE SENSE NOW, wipe tower dimensions are still unknown at this point
	/*{
		// Simulate the brim extrusions, summ the length of the extrusion.
		float e_length = this->tool_change(0, false, PURPOSE_EXTRUDE).total_extrusion_length_in_plane();
		// Shrink wipe_area by the amount of extrusion extruded by the finish_layer().
		// Y stepping of the wipe extrusions.
		float dy = m_line_width;
		// Number of whole wipe lines, that would be extruded to wipe as much material as the finish_layer().
		// Minimum wipe area is 5mm wide.
		//FIXME calculate the purge_lines_width precisely.
		float purge_lines_width = 1.3f;
		wipe_area = std::max(5.f, m_wipe_area - float(floor(e_length / m_wipe_tower_width)) * dy - purge_lines_width);
	}

	this->set_layer(first_layer_height, first_layer_height, tools.size(), true, false);*/
	this->m_num_layer_changes 	= 0;
	this->m_current_tool 		= tools.front();

    // The Prusa i3 MK2 has a working space of [0, -2.2] to [250, 210].
    // Due to the XYZ calibration, this working space may shrink slightly from all directions,
    // therefore the homing position is shifted inside the bed by 0.2 in the firmware to [0.2, -2.0].
//	box_coordinates cleaning_box(xy(0.5f, - 1.5f), m_wipe_tower_width, wipe_area);
	//FIXME: set the width properly
	constexpr float prime_section_width = 60.f;
	box_coordinates cleaning_box(xy(5.f, 0.f), prime_section_width, 15.f);

	PrusaMultiMaterial::Writer writer;
	writer.set_extrusion_flow(m_extrusion_flow)
		  .set_z(m_z_pos)
		  .set_layer_height(m_layer_height)
		  .set_initial_tool(m_current_tool)
		  .append(";--------------------\n"
			 	  "; CP PRIMING START\n")
		  .append(";--------------------\n")
		  .speed_override(100);

	writer.set_initial_position(xy(0.f, 0.f))	// Always move to the starting position
		.travel(cleaning_box.ld, 7200)
		.set_extruder_trimpot(750); 			// Increase the extruder driver current to allow fast ramming.

    // adds tag for analyzer
    char buf[32];
    sprintf(buf, ";%s%d\n", GCodeAnalyzer::Extrusion_Role_Tag.c_str(), erWipeTower);
    writer.append(buf);

	if (purpose == PURPOSE_EXTRUDE || purpose == PURPOSE_MOVE_TO_TOWER_AND_EXTRUDE) {
		for (size_t idx_tool = 0; idx_tool < tools.size(); ++ idx_tool) {
			unsigned int tool = tools[idx_tool];
			m_left_to_right = true;
			toolchange_Change(writer, tool, m_material[tool]); // Select the tool, set a speed override for soluble and flex materials.
			toolchange_Load(writer, cleaning_box); // Prime the tool.
			if (idx_tool + 1 == tools.size()) {
				// Last tool should not be unloaded, but it should be wiped enough to become of a pure color.
				toolchange_Wipe(writer, cleaning_box, 20.f);
			} else {
				// Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
				//writer.travel(writer.x(), writer.y() + m_perimeter_width, 7200);
				toolchange_Wipe(writer, cleaning_box , 5.f);
				box_coordinates box = cleaning_box;
				box.translate(0.f, writer.y() - cleaning_box.ld.y + m_perimeter_width);
				toolchange_Unload(writer, box , m_material[m_current_tool], m_first_layer_temperature[tools[idx_tool + 1]]);
				cleaning_box.translate(prime_section_width, 0.f);
				writer.travel(cleaning_box.ld, 7200);
			}
		    ++ m_num_tool_changes;
		}
	}

	// Reset the extruder current to a normal value.
	writer.set_extruder_trimpot(550)
		  .feedrate(6000)
		  .flush_planner_queue()
		  .reset_extruder()
		  .append("; CP PRIMING END\n"
	 		      ";------------------\n"
				  "\n\n");

	// Force m_idx_tool_change_in_layer to -1, so that tool_change() will know to extrude the wipe tower brim.
	///m_idx_tool_change_in_layer = (unsigned int)(-1);
	m_print_brim = true;

	ToolChangeResult result;
	result.print_z 	  	= this->m_z_pos;
	result.layer_height = this->m_layer_height;
	result.gcode   	  	= writer.gcode();
	result.elapsed_time = writer.elapsed_time();
	result.extrusions 	= writer.extrusions();
	result.start_pos  	= writer.start_pos_rotated();
	result.end_pos 	  	= writer.pos_rotated();
	return result;
}

WipeTower::ToolChangeResult WipeTowerPrusaMM::tool_change(unsigned int tool, bool last_in_layer, Purpose purpose)
{
	// Either it is the last tool unload,
	// or there must be a nonzero wipe tower partitions available.
//	assert(tool < 0 || it_layer_tools->wipe_tower_partitions > 0);

	if ( m_print_brim ) {
	///if (m_idx_tool_change_in_layer == (unsigned int)(-1)) {
		// First layer, prime the extruder.
		return toolchange_Brim(purpose);
	}
	
	float wipe_area = 0.f;
	{/*if (++ m_idx_tool_change_in_layer < (unsigned int)m_max_color_changes && last_in_layer) {
		// This tool_change() call will be followed by a finish_layer() call.
		// Try to shrink the wipe_area to save material, as less than usual wipe is required
		// if this step is foolowed by finish_layer() extrusions wiping the same extruder.
		for (size_t iter = 0; iter < 3; ++ iter) {
			// Simulate the finish_layer() extrusions, summ the length of the extrusion.
			float e_length = 0.f;
			{
				unsigned int old_idx_tool_change = m_idx_tool_change_in_layer;
			    float old_wipe_start_y = m_current_wipe_start_y;
			    m_current_wipe_start_y += wipe_area;
				e_length = this->finish_layer(PURPOSE_EXTRUDE).total_extrusion_length_in_plane();
				m_idx_tool_change_in_layer = old_idx_tool_change;
				m_current_wipe_start_y = old_wipe_start_y;
			}
			// Shrink wipe_area by the amount of extrusion extruded by the finish_layer().
			// Y stepping of the wipe extrusions.
			float dy = m_line_width;
			// Number of whole wipe lines, that would be extruded to wipe as much material as the finish_layer().
			float num_lines_extruded = floor(e_length / m_wipe_tower_width);
			// Minimum wipe area is 5mm wide.
			wipe_area = m_wipe_area - num_lines_extruded * dy;
			if (wipe_area < 5.) {
				wipe_area = 5.;
				break;
			}
		}
	}*/}

	bool last_change_in_layer = false;
	float wipe_volume = 0.f;
	
	// Finds this toolchange info
	if (tool != (unsigned int)(-1))
	{
		for (const auto &b : m_layer_info->tool_changes)
			if ( b.new_tool == tool ) {
				wipe_volume = wipe_volumes[b.old_tool][b.new_tool];
				if (tool == m_layer_info->tool_changes.back().new_tool)
					last_change_in_layer = true;
				wipe_area = b.required_depth * m_layer_info->extra_spacing;
				break;
			}
	}
	else {
		// Otherwise we are going to Unload only. And m_layer_info would be invalid.
	}

	box_coordinates cleaning_box(
		m_wipe_tower_pos + xy(m_perimeter_width / 2.f, m_perimeter_width / 2.f),
		m_wipe_tower_width - m_perimeter_width,
		(tool != (unsigned int)(-1) ? m_layer_info->depth : m_wipe_tower_depth-m_perimeter_width));

	PrusaMultiMaterial::Writer writer;
	writer.set_extrusion_flow(m_extrusion_flow)
		.set_z(m_z_pos)
		.set_layer_height(m_layer_height)
		.set_initial_tool(m_current_tool)
		.set_rotation(m_wipe_tower_pos, m_wipe_tower_width, m_wipe_tower_depth, m_wipe_tower_rotation_angle)
		.set_y_shift(m_y_shift)
		.append(";--------------------\n"
				"; CP TOOLCHANGE START\n")
		.comment_with_value(" toolchange #", m_num_tool_changes)
		.comment_material(m_material[m_current_tool])
		.append(";--------------------\n")
		.speed_override(100);

	xy initial_position = cleaning_box.ld + WipeTower::xy(0.f,m_depth_traversed);

	if (purpose == PURPOSE_MOVE_TO_TOWER || purpose == PURPOSE_MOVE_TO_TOWER_AND_EXTRUDE) {
		// Scaffold leaks terribly, reduce leaking by a full retract when going to the wipe tower.
		float initial_retract = ((m_material[m_current_tool] == SCAFF) ? 1.f : 0.5f) * m_retract;
		writer 	// Lift for a Z hop.
		  	  	.z_hop(m_zhop, 7200)
		  		// Additional retract on move to tower.
		  		.retract(initial_retract, 3600)
		  		// Move to a starting position, one perimeter width inside the cleaning box.
		  		.travel(initial_position, 7200)
		  		// Unlift for a Z hop.
		  		.z_hop_reset(7200)
		  		// Additional retract on move to tower.
		  		.load(initial_retract, 3600)
		  		.load(m_retract, 1500);
	} else {
		// Already at the initial position.
		writer.set_initial_position(initial_position);
	}

    // adds tag for analyzer
    char buf[32];
    sprintf(buf, ";%s%d\n", GCodeAnalyzer::Extrusion_Role_Tag.c_str(), erWipeTower);
    writer.append(buf);

	if (purpose == PURPOSE_EXTRUDE || purpose == PURPOSE_MOVE_TO_TOWER_AND_EXTRUDE) {
		// Increase the extruder driver current to allow fast ramming.
		writer.set_extruder_trimpot(750);
		
		// Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
		if (tool != (unsigned int)-1){ 			// This is not the last change.
			toolchange_Unload(writer, cleaning_box, m_material[m_current_tool],
							  m_is_first_layer ? m_first_layer_temperature[tool] : m_temperature[tool]);			
			toolchange_Change(writer, tool, m_material[tool]); // Change the tool, set a speed override for soluble and flex materials.
			toolchange_Load(writer, cleaning_box);			
			toolchange_Wipe(writer, cleaning_box, wipe_volume); // Wipe the newly loaded filament until the end of the assigned wipe area.

			// Always wipe the nozzle with a long wipe to reduce stringing when moving away from the wipe tower.
			/*box_coordinates box = cleaning_box;
			writer.travel(box.ru, 7200)
					.travel(box.lu);*/
		} else
			toolchange_Unload(writer, cleaning_box, m_material[m_current_tool], m_temperature[m_current_tool]);

		if (last_change_in_layer) // draw perimeter line
			writer.travel(m_wipe_tower_pos, 7000)
				.extrude(m_wipe_tower_pos + xy(0, peters_wipe_tower ? m_wipe_tower_depth : m_layer_info->depth + m_perimeter_width), 3200)
				.extrude(m_wipe_tower_pos + xy(peters_wipe_tower ? m_layer_info->depth + 3*m_perimeter_width : m_wipe_tower_width, peters_wipe_tower ? m_wipe_tower_depth : m_layer_info->depth + m_perimeter_width))
				.extrude(m_wipe_tower_pos + xy(peters_wipe_tower ? m_layer_info->depth + 3*m_perimeter_width : m_wipe_tower_width, 0))
				.extrude(m_wipe_tower_pos);

		// Reset the extruder current to a normal value.
		writer.set_extruder_trimpot(550)
			  .feedrate(6000)
			  .flush_planner_queue()
			  .reset_extruder()
			  .append("; CP TOOLCHANGE END\n"
		 		      ";------------------\n"
					  "\n\n");

	    ++ m_num_tool_changes;
	    //m_current_wipe_start_y += wipe_area;
		m_depth_traversed += wipe_area;
	}

	ToolChangeResult result;
	result.print_z 	  	= this->m_z_pos;
	result.layer_height = this->m_layer_height;
	result.gcode   	  	= writer.gcode();
	result.elapsed_time = writer.elapsed_time();
	result.extrusions 	= writer.extrusions();
	result.start_pos  	= writer.start_pos_rotated();
	result.end_pos 	  	= writer.pos_rotated();
	return result;
}

WipeTower::ToolChangeResult WipeTowerPrusaMM::toolchange_Brim(Purpose purpose, bool sideOnly, float y_offset)
{
	const box_coordinates wipeTower_box(
		m_wipe_tower_pos,
		m_wipe_tower_width,
		m_wipe_tower_depth);

	PrusaMultiMaterial::Writer writer;
	writer.set_extrusion_flow(m_extrusion_flow * 1.1f)
		  .set_z(m_z_pos) // Let the writer know the current Z position as a base for Z-hop.
		  .set_layer_height(m_layer_height)
		  .set_initial_tool(m_current_tool)
  		  .set_rotation(m_wipe_tower_pos, m_wipe_tower_width, m_wipe_tower_depth, m_wipe_tower_rotation_angle)
		  .append(";-------------------------------------\n"
				  "; CP WIPE TOWER FIRST LAYER BRIM START\n");

	xy initial_position = wipeTower_box.lu - xy(m_perimeter_width * 6.f, 0);

	if (purpose == PURPOSE_MOVE_TO_TOWER || purpose == PURPOSE_MOVE_TO_TOWER_AND_EXTRUDE)
		// Move with Z hop.
		writer.z_hop(m_zhop, 7200)
			  .travel(initial_position, 6000)
			  .z_hop_reset(7200);
	else 
		writer.set_initial_position(initial_position);

    // adds tag for analyzer
    char buf[32];
    sprintf(buf, ";%s%d\n", GCodeAnalyzer::Extrusion_Role_Tag.c_str(), erWipeTower);
    writer.append(buf);

	if (purpose == PURPOSE_EXTRUDE || purpose == PURPOSE_MOVE_TO_TOWER_AND_EXTRUDE) {

		writer.extrude_explicit(wipeTower_box.ld - xy(m_perimeter_width * 6.f, 0), // Prime the extruder left of the wipe tower.
								1.5f * m_extrusion_flow * (wipeTower_box.lu.y - wipeTower_box.ld.y), 2400);

		// The tool is supposed to be active and primed at the time when the wipe tower brim is extruded.
		// toolchange_Change(writer, int(tool), m_material[tool]);
		
		// Extrude 4 rounds of a brim around the future wipe tower.
		box_coordinates box(wipeTower_box);			
		box.expand(m_perimeter_width);
		for (size_t i = 0; i < 4; ++ i) {
			writer.travel (box.ld, 7000)
					.extrude(box.lu, 2100).extrude(box.ru)
					.extrude(box.rd      ).extrude(box.ld);
			box.expand(m_perimeter_width);
		}	

		writer.travel(wipeTower_box.ld, 7000); // Move to the front left corner.
		writer.travel(wipeTower_box.rd) // Always wipe the nozzle with a long wipe to reduce stringing when moving away from the wipe tower.
			  .travel(wipeTower_box.ld);
		writer.append("; CP WIPE TOWER FIRST LAYER BRIM END\n"
				      ";-----------------------------------\n");
		// Mark the brim as extruded.
		m_print_brim = false;
	}

	ToolChangeResult result;
	result.print_z 	  	= this->m_z_pos;
	result.layer_height = this->m_layer_height;
	result.gcode   	  	= writer.gcode();
	result.elapsed_time = writer.elapsed_time();
	result.extrusions 	= writer.extrusions();
	result.start_pos  	= writer.start_pos_rotated();
	result.end_pos 	  	= writer.pos_rotated();
	return result;
}



// Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
void WipeTowerPrusaMM::toolchange_Unload(
	PrusaMultiMaterial::Writer &writer,
	const box_coordinates 	&cleaning_box,
	const material_type		 current_material,
	const int 				 new_temperature)
{
	float xl = cleaning_box.ld.x + 1.f * m_perimeter_width;
	float xr = cleaning_box.rd.x - 1.f * m_perimeter_width;
	
	writer.append("; CP TOOLCHANGE UNLOAD\n");
	
	const float line_width = m_line_width * ramming_line_width_multiplicator; // desired ramming line thickness
	const float y_step = line_width * ramming_step_multiplicator * m_extra_spacing;			  // spacing between lines in mm

	unsigned i = 0;										// iterates through ramming_speed
	m_left_to_right = true;								// current direction of ramming
	float remaining = xr - xl ;							// keeps track of distance to the next turnaround
	float e_done = 0;									// measures E move done from each segment
	
	writer.travel(xl, cleaning_box.ld.y + m_depth_traversed + y_step/2.f ); // move to starting position

	while (i < ramming_speed.size())
	{
		const float x = volume_to_length(ramming_speed[i] * 0.25f, line_width, m_layer_height);
		const float e = ramming_speed[i] * 0.25f / Filament_Area; // transform volume per sec to E move;
		const float dist = std::min(x - e_done, remaining);		  // distance to travel for either the next 0.25s, or to the next turnaround
		const float actual_time = dist/x * 0.25;
		writer.ram(writer.x(), writer.x() + (m_left_to_right ? 1.f : -1.f) * dist, 0, 0, e * (dist / x), std::hypot(dist, e * (dist / x)) / (actual_time / 60.));
		remaining -= dist;

		if (remaining < WT_EPSILON)	{ // we reached a turning point
			writer.travel(writer.x(), writer.y() + y_step, 7200);
			m_left_to_right = !m_left_to_right;
			remaining = xr - xl;
		}
		e_done += dist; // subtract what was actually done
		if (e_done > x - WT_EPSILON) { // current segment finished
			++i;
			e_done = 0;
		}
	}
	WipeTower::xy end_of_ramming(writer.x(),writer.y());

// Alex's old ramming:
{
  /*switch (current_material)
	{
	case ABS:
   		// ramming          start                    end                  y increment     amount feedrate
		writer.ram(xl + m_perimeter_width * 2, xr - m_perimeter_width,     y_step * 0.2f, 0,  1.2f  * e, 4000)
			  .ram(xr - m_perimeter_width,     xl + m_perimeter_width,     y_step * 1.2f, e0, 1.6f  * e, 4600)
			  .ram(xl + m_perimeter_width * 2, xr - m_perimeter_width * 2, y_step * 1.2f, e0, 1.8f  * e, 5000)
			  .ram(xr - m_perimeter_width * 2, xl + m_perimeter_width * 2, y_step * 1.2f, e0, 1.8f  * e, 5000);
		break;
	case PVA:
		// Used for the PrimaSelect PVA
		writer.ram(xl + m_perimeter_width * 2, xr - m_perimeter_width,     y_step * 0.2f, 0,  1.75f * e, 4000)
			  .ram(xr - m_perimeter_width,     xl + m_perimeter_width,     y_step * 1.5f, 0,  1.75f * e, 4500)
			  .ram(xl + m_perimeter_width * 2, xr - m_perimeter_width * 2, y_step * 1.5f, 0,  1.75f * e, 4800)
			  .ram(xr - m_perimeter_width,     xl + m_perimeter_width,     y_step * 1.5f, 0,  1.75f * e, 5000);
		break;
	case SCAFF:
		writer.ram(xl + m_perimeter_width * 2, xr - m_perimeter_width,     y_step * 2.f,  0,  1.75f * e, 4000)
			  .ram(xr - m_perimeter_width,     xl + m_perimeter_width,     y_step * 3.f,  0,  2.34f * e, 4600)
			  .ram(xl + m_perimeter_width * 2, xr - m_perimeter_width * 2, y_step * 3.f,  0,  2.63f * e, 5200);
		break;
	default:
		// PLA, PLA/PHA and others
		// Used for the Verbatim BVOH, PET, NGEN, co-polyesters
		writer.ram(xl + m_perimeter_width * 2, xr - m_perimeter_width,     y_step * 0.2f, 0,  1.60f * e, 4000)
			  .ram(xr - m_perimeter_width,     xl + m_perimeter_width,     y_step * 1.2f, e0, 1.65f * e, 4600)
			  .ram(xl + m_perimeter_width * 2, xr - m_perimeter_width * 2, y_step * 1.2f, e0, 1.74f * e, 5200);
	}*/
}
	// Pull the filament end into a cooling tube.
	writer.retract(15, 5000).retract(50, 5400).retract(15, 3000).retract(12, 2000);

	if (new_temperature != 0) 	// Set the extruder temperature, but don't wait.
		writer.set_extruder_temp(new_temperature, false);

// cooling:
	writer.retract(2, 2000);
	writer.suppress_preview();
	writer.travel(writer.x(), writer.y() + y_step);
	const float start_x = writer.x();
	const float turning_point = ( xr-start_x > start_x-xl ? xr : xl );
	const float max_x_dist = 2*std::abs(start_x-turning_point);
	const int N = 4 + (cooling_time-14)/3;
	float time = cooling_time / N;
	
	i = 0;
	while (i<N) {		
		const float speed = std::min(3.4,2.2 + i*0.3 + (i==0 ? 0 : 0.3)); // mm per second: 2.2, 2.8, 3.1, 3.4, 3.4, 3.4, ...		
		const float e_dist = std::min(speed * time,10.f); // distance to travel
		
		if (speed * time < 10.f) { 	// this move is the last one at this speed
			++i;
			time = cooling_time / N;			
		}
		else
			time -= e_dist / speed; // subtract time this part will really take
		
		// as for x, we will make sure the feedrate is at most 2000
		float x_dist = (turning_point - WT_EPSILON < xl ? -1.f : 1.f) * std::min(e_dist * (float)sqrt(pow(2000 / (60 * speed), 2) - 1),max_x_dist);
		const float feedrate = std::hypot(e_dist, x_dist) / ((e_dist / speed) / 60.f);
		writer.cool(start_x+x_dist/2.f,start_x,e_dist/2.f,-e_dist/2.f, feedrate);
	}
	writer.retract(-2, 2000);
	writer.travel(writer.x(), writer.y() - y_step,2400);

	// Alex's old cooling:
	{
		// In case the current print head position is closer to the left edge, reverse the direction.
		/*if (std::abs(writer.x() - xl) < std::abs(writer.x() - xr))
		std::swap(xl, xr);
		// Horizontal cooling moves will be performed at the following Y coordinate:
		writer.travel(xr, writer.y() + y_step * 0.8f, 7200)
		  .suppress_preview();
		switch (current_material)
		{
		case PVA:
			writer.cool(xl, xr, 3, -5, 1600)
				.cool(xl, xr, 5, -5, 2000)
				.cool(xl, xr, 5, -5, 2200)
				.cool(xl, xr, 5, -5, 2400)
				.cool(xl, xr, 5, -5, 2400)
				.cool(xl, xr, 5, -3, 2400);
			break;
		case SCAFF:
			writer.cool(xl, xr, 3, -5, 1600)
				.cool(xl, xr, 5, -5, 2000)
				.cool(xl, xr, 5, -5, 2200)
				.cool(xl, xr, 5, -5, 2200)
				.cool(xl, xr, 5, -3, 2400);
			break;
		default:
			writer.cool(xl, xr, 3, -5, 1600)
				.cool(xl, xr, 5, -5, 2000)
				.cool(xl, xr, 5, -5, 2400)
				.cool(xl, xr, 5, -3, 2400);
		}*/
}
	// this is to align ramming and future loading extrusions, so the future y-steps can be uniform from the start:
	writer.travel(end_of_ramming.x, end_of_ramming.y + (y_step/m_extra_spacing-m_line_width) / 2.f, 2400.f);

	writer.resume_preview()
		  .flush_planner_queue();
}

// Change the tool, set a speed override for soluble and flex materials.
void WipeTowerPrusaMM::toolchange_Change(
	PrusaMultiMaterial::Writer &writer,
	const unsigned int 	new_tool, 
	material_type 		new_material)
{
	// Speed override for the material. Go slow for flex and soluble materials.
	int speed_override;
	switch (new_material) {
	case PVA:   speed_override = (m_z_pos < 0.80f) ? 60 : 80; break;
	case SCAFF: speed_override = 35; break;
	case FLEX:  speed_override = 35; break;
	default:    speed_override = 100;
	}
	writer.set_tool(new_tool)
	      .speed_override(speed_override)
	      .flush_planner_queue();
	m_current_tool = new_tool;
}



void WipeTowerPrusaMM::toolchange_Load(
	PrusaMultiMaterial::Writer &writer,
	const box_coordinates  &cleaning_box)
{	
	float xl = cleaning_box.ld.x + m_perimeter_width * 0.75f;
	float xr = cleaning_box.rd.x - m_perimeter_width * 0.75f;
	float oldx=writer.x();	// the nozzle is in place to do the first wiping moves, we will remember the position
	float oldy=writer.y();

	writer.append("; CP TOOLCHANGE LOAD\n")
	// Load the filament while moving left / right,
	// so the excess material will not create a blob at a single position.
		  .suppress_preview()
		  // Accelerate the filament loading
		  .load_move_x(xr, 20, 1400)
		  // Fast loading phase
		  .load_move_x(xl, 40, 3000)
		  // Slowing down
		  .load_move_x(xr, 20, 1600)
		  .load_move_x(xl, 10, 1000)
		  .travel(oldx,oldy)
		  .resume_preview();

// Alex's old loading extrusions - this has been moved to toolchange_Wipe(...)
{
	// Extrude first five lines (just three lines if colorInit is set).
	/*writer.extrude(xr, writer.y(), 1600);
	bool   colorInit = false;
	size_t pass = colorInit ? 1 : 2;
	float dy = ((m_current_shape == SHAPE_NORMAL) ? 1.f : -1.f) * m_line_width;
	for (int i = 0; i < pass; ++ i) {
		writer.travel (xr, writer.y() + dy, 7200);
		writer.extrude(xl, writer.y(), 		2200);
		writer.travel (xl, writer.y() + dy, 7200);
	 	writer.extrude(xr, writer.y(), 		2200);
	}*/	
}
	// Reset the extruder current to the normal value.
	writer.set_extruder_trimpot(550);
}




// Wipe the newly loaded filament until the end of the assigned wipe area.
void WipeTowerPrusaMM::toolchange_Wipe(
	PrusaMultiMaterial::Writer &writer,
	const box_coordinates  &cleaning_box,
	float wipe_volume)
{
	// Increase flow on first layer, slow down print.
	writer.set_extrusion_flow(m_extrusion_flow * (m_is_first_layer ? 1.18f : 1.f))
		  .append("; CP TOOLCHANGE WIPE\n");
	float wipe_coeff = m_is_first_layer ? 0.5f : 1.f;
	const float& xl = cleaning_box.ld.x;
	const float& xr = cleaning_box.rd.x;
	
	// DEBUGGING: The function makes sure it always wipes at least the ordered volume, even if it means violating
	//            the perimeter. This can later be removed and simply wipe until the end of the assigned area.
	//			  (Variables x_to_wipe and traversed_x)

	float x_to_wipe = volume_to_length(wipe_volume, m_line_width, m_layer_height);
	float dy = m_extra_spacing*m_line_width;
	float wipe_speed = 1600.f;

	for (int i = 0; true; ++i)	{
		if (i!=0) {
			if (wipe_speed < 1610.f) wipe_speed = 1800.f;
			else if (wipe_speed < 1810.f) wipe_speed = 2200.f;
			else if (wipe_speed < 2210.f) wipe_speed = 4200.f;
			else wipe_speed = std::min(4800.f, wipe_speed + 50.f);
		}
		
		float traversed_x = writer.x();
		if (m_left_to_right)
			writer.extrude(xr - (i % 4 == 0 ? 0 : 1.5*m_perimeter_width), writer.y(), wipe_speed * wipe_coeff);
		else
			writer.extrude(xl + (i % 4 == 1 ? 0 : 1.5*m_perimeter_width), writer.y(), wipe_speed * wipe_coeff);
		
		/*if ((m_current_shape == SHAPE_NORMAL) ?		// in case next line would not fit
			(writer.y() > cleaning_box.lu.y - m_perimeter_width * 1.5f) :
			(writer.y() < cleaning_box.ld.y + m_perimeter_width * 1.5f))
			break;*/
		traversed_x -= writer.x();
		x_to_wipe -= fabs(traversed_x);
		if (x_to_wipe < WT_EPSILON) {
			writer.travel(m_left_to_right ? xl + 1.5*m_perimeter_width : xr - 1.5*m_perimeter_width, writer.y(), 7200);
			break;
		}
		// stepping to the next line:
		writer.extrude(writer.x() + (i % 4 == 0 ? -1.f : (i % 4 == 1 ? 1.f : 0.f)) * 1.5*m_perimeter_width, writer.y() + dy);
		m_left_to_right = !m_left_to_right;
	}
	writer.set_extrusion_flow(m_extrusion_flow); // Reset the extrusion flow.

	// Wipe the nozzle

}




WipeTower::ToolChangeResult WipeTowerPrusaMM::finish_layer(Purpose purpose)
{
	// This should only be called if the layer is not finished yet.
	// Otherwise the caller would likely travel to the wipe tower in vain.
	assert(! this->layer_finished());

	PrusaMultiMaterial::Writer writer;
	writer.set_extrusion_flow(m_extrusion_flow)
		.set_z(m_z_pos)
		.set_layer_height(m_layer_height)
		.set_initial_tool(m_current_tool)
		.set_rotation(m_wipe_tower_pos, m_wipe_tower_width, m_wipe_tower_depth, m_wipe_tower_rotation_angle)
		.set_y_shift(m_y_shift)
		.append(";--------------------\n"
				"; CP EMPTY GRID START\n")
		// m_num_layer_changes is incremented by set_z, so it is 1 based.
		.comment_with_value(" layer #", m_num_layer_changes - 1);

	// Slow down on the 1st layer.
	float speed_factor = m_is_first_layer ? 0.5f : 1.f;
	float current_depth = m_layer_info->depth - m_layer_info->toolchanges_depth();
	box_coordinates fill_box(m_wipe_tower_pos + xy(m_perimeter_width, m_depth_traversed + m_perimeter_width),
							 m_wipe_tower_width - 2 * m_perimeter_width, current_depth-m_perimeter_width);

	if (purpose == PURPOSE_MOVE_TO_TOWER || purpose == PURPOSE_MOVE_TO_TOWER_AND_EXTRUDE) {
		if ( m_layer_info->tool_changes.size() == 0) {
			// There were no tool changes at all in this layer.
			writer.retract(m_retract * 1.5f, 3600)
				  // Jump with retract to fill_box.ld + a random shift in +x.
				  .z_hop(m_zhop, 7200)
				  .travel(fill_box.ld + xy(5.f + 15.f * float(rand()) / RAND_MAX, 0.f), 7000)
				  .z_hop_reset(7200)
				  // Prime the extruder.
				  .load_move_x(fill_box.ld.x, m_retract * 1.5f, 3600);
		} else {
			// Otherwise the extruder is already over the wipe tower.
		}
	} else {
		// The print head is inside the wipe tower. Rather move to the start of the following extrusion.
		writer.set_initial_position(fill_box.ld);
	}

	if (purpose == PURPOSE_EXTRUDE || purpose == PURPOSE_MOVE_TO_TOWER_AND_EXTRUDE) {
		box_coordinates box = fill_box;
		for (int i=0;i<2;++i) {
			if (m_layer_info->toolchanges_depth() < WT_EPSILON) { // there were no toolchanges on this layer
				if (i==0) box.expand(m_perimeter_width);
				else box.expand(-m_perimeter_width);
			}
			else i=2;	// only draw the inner perimeter

			writer.travel(box.ld,7200)
			    .extrude(box.lu, 2400 * speed_factor)
			    .extrude(box.ru)
			    .extrude(box.rd)
			    .extrude(box.ld);
		}

		if (m_is_first_layer && improve_first_layer_adhesion) {
			// Extrude a dense infill at the 1st layer to improve 1st layer adhesion of the wipe tower.
			box.expand(-m_perimeter_width/2.f);
			unsigned nsteps = int(floor((box.lu.y - box.ld.y) / (2*m_perimeter_width)));
			float step   = (box.lu.y - box.ld.y) / nsteps;
			writer.travel(box.ld-xy(m_perimeter_width/2.f,m_perimeter_width/2.f));
			if (nsteps >= 0)
				for (size_t i = 0; i < nsteps; ++i)	{
					writer.extrude(box.ld.x+m_perimeter_width/2.f, writer.y() + 0.5f * step);
					writer.extrude(box.rd.x - m_perimeter_width / 2.f, writer.y());
					writer.extrude(box.rd.x - m_perimeter_width / 2.f, writer.y() + 0.5f * step);
					writer.extrude(box.ld.x + m_perimeter_width / 2.f, writer.y());
				}
				writer.travel(box.rd.x-m_perimeter_width/2.f,writer.y()); // wipe the nozzle
		} else {
			// Extrude a sparse infill to support the material to be printed above.

			const float dy = (fill_box.lu.y - fill_box.ld.y - m_perimeter_width);
			if (dy > m_perimeter_width)
			{
				// Extrude an inverse U at the left of the region.
				writer.travel(fill_box.ld + xy(m_perimeter_width * 2, 0.f))
					  .extrude(fill_box.lu + xy(m_perimeter_width * 2, 0.f), 2900 * speed_factor);

				const float left = fill_box.lu.x+2*m_perimeter_width;
				const float right = fill_box.ru.x - 2 * m_perimeter_width;
				const int n = 1+(right-left)/max_bridge_distance;
				const float dx = (right-left)/n;
				for (int i=1;i<=n;++i) {
					float x=left+dx*i;
					writer.travel(x,writer.y());
					writer.extrude(x,i%2 ? fill_box.rd.y : fill_box.ru.y);					
				}
				writer.travel(left,writer.y(),7200); // wipes the nozzle before moving away from the wipe tower
			}
		}
/*
		// if (purpose == PURPOSE_MOVE_TO_TOWER_AND_EXTRUDE)
		if (true)
	       	// Wipe along the front side of the current wiping box.
			// Always wipe the nozzle with a long wipe to reduce stringing when moving away from the wipe tower.
			writer.travel(fill_box.ld + xy(  m_perimeter_width, m_perimeter_width / 2), 7200)
			  	  .travel(fill_box.rd + xy(- m_perimeter_width, m_perimeter_width / 2));
		else
			writer.feedrate(7200);
*/
		writer.append("; CP EMPTY GRID END\n"
				      ";------------------\n\n\n\n\n\n\n");

		// Indicate that this wipe tower layer is fully covered.
		m_depth_traversed = m_wipe_tower_depth-m_perimeter_width;
	    ///m_idx_tool_change_in_layer = (unsigned int)m_max_color_changes;		
	}
	ToolChangeResult result;
	result.print_z 	  	= this->m_z_pos;
	result.layer_height = this->m_layer_height;
	result.gcode   	  	= writer.gcode();
	result.elapsed_time = writer.elapsed_time();
	result.extrusions 	= writer.extrusions();
	result.start_pos 	= writer.start_pos_rotated();
	result.end_pos 	  	= writer.pos_rotated();
	return result;
}

// Appends a toolchange into m_plan and calculates neccessary depth of the corresponding box
void WipeTowerPrusaMM::plan_toolchange(float z_par, float layer_height_par, unsigned int old_tool, unsigned int new_tool,bool brim)
{	
	assert(m_plan.back().z <= z_par + WT_EPSILON );	// refuses to add a layer below the last one

	float width = m_wipe_tower_width - 3*m_perimeter_width; 
	float length_to_extrude = volume_to_length(0.25f * std::accumulate(ramming_speed.begin(), ramming_speed.end(), 0.f),
										  m_line_width * ramming_line_width_multiplicator,
										  layer_height_par);
	float depth = (int(length_to_extrude / width) + 1) * (m_line_width * ramming_line_width_multiplicator * ramming_step_multiplicator);
	length_to_extrude = width*((length_to_extrude / width)-int(length_to_extrude / width)) - width;
	length_to_extrude += volume_to_length(wipe_volumes[old_tool][new_tool], m_line_width, layer_height_par);
	length_to_extrude = std::max(length_to_extrude,0.f);
	depth += (int(length_to_extrude / width) + 1) * m_line_width;
	depth *= m_extra_spacing;

	if (m_plan.empty() || m_plan.back().z + WT_EPSILON < z_par) // if we moved to a new layer, we'll add it to m_plan along with the first toolchange
		m_plan.push_back(WipeTowerInfo(z_par, layer_height_par));

	if ( brim || old_tool != new_tool )	{
		if (brim) // this toolchange prints brim, we need it in m_plan, but not to count its depth
			depth = 0.f;
		m_plan.back().tool_changes.push_back(WipeTowerInfo::ToolChange(old_tool, new_tool, depth));
	}

	// Calculate m_wipe_tower_depth (maximum depth for all the layers) and propagate depths downwards
	/*float this_layer_depth = m_plan.back().toolchanges_depth();
	m_plan.back().depth = this_layer_depth;
	
	if (this_layer_depth > m_wipe_tower_depth - m_perimeter_width)
		m_wipe_tower_depth = this_layer_depth + m_perimeter_width;
	for (int i = m_plan.size() - 2; i >= 0 && m_plan[i].depth < this_layer_depth; i-- ) {
		if ( this_layer_depth - m_plan[i].depth < min_layer_difference && !m_plan[i].tool_changes.empty())
			m_plan[i].depth += this_layer_depth - m_plan[i].depth;
		m_plan[i].depth = this_layer_depth;
	}*/
}



void WipeTowerPrusaMM::plan_tower()
{
	// Calculate m_wipe_tower_depth (maximum depth for all the layers) and propagate depths downwards
	m_wipe_tower_depth = 0.f;
	for (auto& layer : m_plan)
		layer.depth = 0.f;
	
	for (int layer_index = m_plan.size() - 1; layer_index >= 0; --layer_index)
	{
		float this_layer_depth = std::max(m_plan[layer_index].depth, m_plan[layer_index].toolchanges_depth());
		m_plan[layer_index].depth = this_layer_depth;
		
		if (this_layer_depth > m_wipe_tower_depth - m_perimeter_width)
			m_wipe_tower_depth = this_layer_depth + m_perimeter_width;

		for (int i = layer_index - 1; i >= 0 /*&& m_plan[i].depth < this_layer_depth*/; i--)
		{
			if (m_plan[i].depth - this_layer_depth < min_layer_difference )
				m_plan[i].depth = this_layer_depth;
		}
	}
}


void WipeTowerPrusaMM::make_wipe_tower_square()
{
	const float width = m_wipe_tower_width - 3 * m_perimeter_width;
	const float depth = m_wipe_tower_depth - m_perimeter_width;
	// area that we actually print into is width*depth
	float side = sqrt(depth * width);

	m_wipe_tower_width = side + 3 * m_perimeter_width;
	m_wipe_tower_depth = side + 2 * m_perimeter_width;
	// For all layers, find how depth changed and update all toolchange depths
	for (auto &lay : m_plan)
	{
		side = sqrt(lay.depth * width);
		float width_ratio = width / side;

		//lay.extra_spacing = width_ratio;
		for (auto &tch : lay.tool_changes)
			tch.required_depth *= width_ratio;
	}

	plan_tower();				// propagates depth downwards again (width has changed)
	for (auto& lay : m_plan)	// depths set, now the spacing
		lay.extra_spacing = lay.depth / lay.toolchanges_depth();

}


// Processes vector m_plan and calls respective functions to generate G-code for the wipe tower
// Resulting ToolChangeResults are appended into vector "result"
void WipeTowerPrusaMM::generate(std::vector<std::vector<WipeTower::ToolChangeResult>> &result)
{
	m_extra_spacing = 1.f;

	plan_tower();
	if (peters_wipe_tower)
			make_wipe_tower_square();

	std::vector<WipeTower::ToolChangeResult> layer_result;
	for (auto layer : m_plan)
	{
		set_layer(layer.z,layer.height,0,layer.z == m_plan.front().z,layer.z == m_plan.back().z);


		if (peters_wipe_tower)
			m_wipe_tower_rotation_angle += 90.f;
		else
			m_wipe_tower_rotation_angle += 180.f;
		if (!peters_wipe_tower && m_layer_info->depth < m_wipe_tower_depth - m_perimeter_width)
			m_y_shift = (m_wipe_tower_depth-m_layer_info->depth-m_perimeter_width)/2.f;



		for (const auto &toolchange : layer.tool_changes)
			layer_result.emplace_back(tool_change(toolchange.new_tool, false, WipeTower::PURPOSE_EXTRUDE));

		if (! layer_finished()) {
			layer_result.emplace_back(finish_layer(WipeTower::PURPOSE_EXTRUDE));
			if (layer_result.size() > 1) {
				// Merge the two last tool changes into one.
				WipeTower::ToolChangeResult &tc1 = layer_result[layer_result.size() - 2];
				WipeTower::ToolChangeResult &tc2 = layer_result.back();
				if (tc1.end_pos != tc2.start_pos) {
					// Add a travel move from tc1.end_pos to tc2.start_pos.
					char buf[2048];
					sprintf(buf, "G1 X%.3f Y%.3f F7200\n", tc2.start_pos.x, tc2.start_pos.y);
					tc1.gcode += buf;
				}
				tc1.gcode += tc2.gcode;
				tc1.extrusions.insert(tc1.extrusions.end(), tc2.extrusions.begin(), tc2.extrusions.end());
				tc1.end_pos = tc2.end_pos;
				layer_result.pop_back();
			}			
		}

		result.emplace_back(std::move(layer_result));
		m_is_first_layer = false;
	}
}


}; // namespace Slic3r
