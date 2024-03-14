/******************************************************************************
License: BSD Zero Clause License
Copyright:
	(C) 2008 - 2021 Alexander Shaduri <ashaduri@gmail.com>
******************************************************************************/
/// \file
/// \author Alexander Shaduri
/// \ingroup rconfig_examples
/// \weakgroup rconfig_examples
/// @{

// disable libdebug, we don't link to it
#undef HZ_USE_LIBDEBUG
#define HZ_USE_LIBDEBUG 0
// enable libdebug emulation through std::cerr
#undef HZ_EMULATE_LIBDEBUG
#define HZ_EMULATE_LIBDEBUG 1

#include "rconfig/loadsave.h"

#if defined ENABLE_GLIB && ENABLE_GLIB
	#include "rconfig/autosave.h"
#endif
#include "hz/main_tools.h"

#include <iostream>
#include <cstdint>
#include <chrono>



/// Main function for the test
int main()
{
	return hz::main_exception_wrapper([]()
	{
		using namespace std::literals;

		rconfig::load_from_file("test.config");

		// populate /default:
		rconfig::set_default_data("app/use_stuff", true);  // bool
		rconfig::set_default_data("app/some_string1", std::string("some_string1_data"));
		rconfig::set_default_data("app/some_string2", "some_string2_data");  // this will store it as std::string
		rconfig::set_default_data("app/int_value", 5);  // stored as int64_t
		rconfig::set_default_data("app/int64_value", int64_t(5));  // explicitly
		rconfig::set_default_data("app/double_value", 6.7);  // double

		rconfig::set_data("app/int_var", 11);  // override default.

		const int int_var = rconfig::get_data<int>("app/int_value");
		std::cerr << "app/int_value: " << int_var << "\n";

		std::cerr << "app/some_string2: " << rconfig::get_data<std::string>("app/some_string2") << "\n";

		rconfig::dump_config();
		rconfig::save_to_file("test.config");

	#if defined ENABLE_GLIB && ENABLE_GLIB
		rconfig::autosave_set_config_file("test2.config");
		rconfig::autosave_start(2s);  // every 2 seconds
		while(true) {  // FIXME This is undefined behavior
			// without this the timeout function won't be called.
			g_main_context_iteration(nullptr, FALSE);
		}
	#endif

		return EXIT_SUCCESS;
	});
}







/// @}
