/******************************************************************************
License: Zlib
Copyright:
	(C) 2008 - 2021 Alexander Shaduri <ashaduri@gmail.com>
******************************************************************************/
/// \file
/// \author Alexander Shaduri
/// \ingroup libdebug
/// \weakgroup libdebug
/// @{

#include <string>
#include <iosfwd>  // std::ostream definition
#include <sstream>

#include "dout.h"
#include "dflags.h"
#include "dstate.h"
#include "dexcept.h"




// This may throw for invalid domain or level.
std::ostream& debug_out(debug_level::flag level, const std::string& domain)
{
	auto& dm = debug_internal::get_debug_state_ref().get_domain_map_ref();

	auto level_map = dm.find(domain);
	if (level_map == dm.end()) {  // no such domain
		// this is an internal error
		const std::string msg = "debug_out(): Debug state doesn't contain the requested domain: \"" + domain + "\".";
		throw debug_internal_error(msg.c_str());
	}

	auto os = level_map->second.find(level);
	if (level_map == dm.end()) {
		const std::string msg = std::string("debug_out(): Debug state doesn't contain the requested level ") +
				debug_level::get_name(level) + " in domain: \"" + domain + "\".";

		// this is an internal error
		throw debug_internal_error(msg.c_str());
	}

	return *(os->second);
}




// Start / stop prefix printing. Useful for large dumps

void debug_begin()
{
	debug_internal::get_debug_state_ref().push_inside_begin();
}


void debug_end()
{
	debug_internal::get_debug_state_ref().pop_inside_begin();
	// this is needed because else the contents won't be written until next write.
	debug_internal::get_debug_state_ref().force_output();
}





namespace debug_internal {


	std::string DebugSourcePos::str() const
	{
		std::ostringstream os;
		os << "(";

		if (enabled_types.test(debug_pos::func_name)) {
			os << "function: " << func_name;

		} else if (enabled_types.test(debug_pos::func)) {
			os << "function: " << func << "()";
		}

		if (enabled_types.test(debug_pos::file)) {
			if (os.str() != "(")
				os << ", ";
			os << "file: " << file;
		}

		if (enabled_types.test(debug_pos::line)) {
			if (os.str() != "(")
				os << ", ";
			os << "line: " << line;
		}

		os << ")";

		return os.str();
	}


}



// ------------------ Indentation and manipulators


// increase indentation level for all debug levels
void debug_indent_inc(int by)
{
	const int curr = debug_internal::get_debug_state_ref().get_indent_level();
	debug_internal::get_debug_state_ref().set_indent_level(curr + by);
}


void debug_indent_dec(int by)
{
	int curr = debug_internal::get_debug_state_ref().get_indent_level();
	curr -= by;
	if (curr < 0)
		curr = 0;
	debug_internal::get_debug_state_ref().set_indent_level(curr);
}


void debug_indent_reset()
{
	debug_internal::get_debug_state_ref().set_indent_level(0);
}








/// @}
