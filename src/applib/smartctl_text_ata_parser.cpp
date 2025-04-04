/******************************************************************************
License: GNU General Public License v3.0 only
Copyright:
	(C) 2008 - 2021 Alexander Shaduri <ashaduri@gmail.com>
******************************************************************************/
/// \file
/// \author Alexander Shaduri
/// \ingroup applib
/// \weakgroup applib
/// @{

#include "smartctl_text_ata_parser.h"

// #include <glibmm.h>
#include <chrono>
#include <clocale>  // localeconv
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fmt/format.h"
// #include "hz/locale_tools.h"  // ScopedCLocale, locale_c_get().
#include "storage_property.h"
#include "hz/string_algo.h"  // string_*
#include "hz/string_num.h"  // string_is_numeric, number_to_string
#include "hz/debug.h"  // debug_*

#include "app_regex.h"
//#include "ata_storage_property_descr.h"
// #include "warning_colors.h"
#include "smartctl_parser_types.h"
#include "smartctl_version_parser.h"
#include "smartctl_text_parser_helper.h"



namespace {


	/// Get storage property by checksum error name (which corresponds to
	/// an output section).
	inline StorageProperty app_get_checksum_error_property(const std::string& reported_section_name)
	{
		StorageProperty p;
		std::string disp_name = "Error in " + reported_section_name + " structure";

		if (reported_section_name == "Attribute Data") {
			p.section = StoragePropertySection::AtaAttributes;
			p.set_name("_text_only/attribute_data_checksum_error", disp_name);

		} else if (reported_section_name == "Attribute Thresholds") {
			p.section = StoragePropertySection::AtaAttributes;
			p.set_name("_text_only/attribute_thresholds_checksum_error", disp_name);

		} else if (reported_section_name == "ATA Error Log") {
			p.section = StoragePropertySection::AtaErrorLog;
			p.set_name("_text_only/ata_error_log_checksum_error", disp_name);

		} else if (reported_section_name == "Self-Test Log") {
			p.section = StoragePropertySection::SelftestLog;
			p.set_name("_text_only/selftest_log_checksum_error", disp_name);
		}

		p.reported_value = "checksum error";
		p.value = p.reported_value;  // string-type value

		return p;
	}


}



// Parse full "smartctl -x" output
hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse(std::string_view smartctl_output)
{
	// -------------------- Fix the output, so it doesn't interfere with proper parsing

	// perform any2unix
	std::string s = hz::string_trim_copy(hz::string_any_to_unix_copy(smartctl_output));

	if (s.empty()) {
		debug_out_warn("app", DBG_FUNC_MSG << "Empty string passed as an argument. Returning.\n");
		return hz::Unexpected(SmartctlParserError::EmptyInput, "Smartctl data is empty.");
	}


	// The first line may be a command, filter it out. e.g.
	// # smartctl -a /dev/sda
	// NO NEED: We ignore everything non-section (except version info).
	// Note: We ignore non-section lines, so we don't need any filtering here.
// 	{
// 		app_regex_replace_once("/^# .*$/", "", s);  // replace first only, on the first line only.
// 	}


	// Checksum warnings are kind of randomly distributed, so
	// extract and remove them.
	{
		const auto re = app_regex_re("/\\nWarning! SMART (.+) Structure error: invalid SMART checksum\\.$/mi");
		for (auto it = std::sregex_iterator(s.begin(), s.end(), re), end = std::sregex_iterator(); it != end; ++it) {
			const std::string structure_name = hz::string_trim_copy(it->str(1));
			add_property(app_get_checksum_error_property(structure_name));
		}
		app_regex_replace(re, "", s);  // remove them from s.
	}

	// Remove some additional stuff which doesn't fit
	// Display this warning somewhere? (info section?)
	// Or not, these options don't do anything crucial - just some translation stuff.
	{
		app_regex_replace("/\\n.*May need -F samsung or -F samsung2 enabled; see manual for details\\.$/mi",
				"", s);  // remove from s
	}


	// The Warning: parts also screw up newlines sometimes (making double-newlines,
	// confusing for section separation).
	{
		const auto re = app_regex_re("/^(Warning: ATA error count.*\\n)\\n/mi");

		std::string match;
		if (app_regex_partial_match(re, s, &match)) {
			app_regex_replace(re, match, s);  // make one newline less
		}
	}


	// If the device doesn't support many things, the warnings aren't separated (for sections).
	// Fix that. This affects old smartctl only (at least 6.5 fixed the warnings).
	{
		const auto re1 = app_regex_re("/^(Warning: device does not support Error Logging)$/mi");
		const auto re2 = app_regex_re("/^(Warning: device does not support Self Test Logging)$/mi");
		const auto re3 = app_regex_re("/^(Device does not support Selective Self Tests\\/Logging)$/mi");
		const auto re4 = app_regex_re("/^(Warning: device does not support SCT Commands)$/mi");
		std::string match;

		if (app_regex_partial_match(re1, s, &match))
			app_regex_replace(re1, "\n" + match + "\n", s);  // add extra newlines

		if (app_regex_partial_match(re2, s, &match))
			app_regex_replace(re2, "\n" + match + "\n", s);  // add extra newlines

		if (app_regex_partial_match(re3, s, &match))
			app_regex_replace(re3, "\n" + match + "\n", s);  // add extra newlines

		if (app_regex_partial_match(re4, s, &match))
			app_regex_replace(re4, "\n" + match + "\n", s);  // add extra newlines
	}

	// Some errors get in the way of subsection detection and have little value, remove them.
	{
		// "ATA_READ_LOG_EXT (addr=0x00:0x00, page=0, n=1) failed: 48-bit ATA commands not implemented"
		// or "ATA_READ_LOG_EXT (addr=0x11:0x00, page=0, n=1) failed: scsi error aborted command"
		// in front of "Read GP Log Directory failed" and "Read SATA Phy Event Counters failed".
		const auto re1 = app_regex_re("/^(ATA_READ_LOG_EXT \\([^)]+\\) failed: .*)$/mi");
		// "SMART WRITE LOG does not return COUNT and LBA_LOW register"
		// in front of "SCT (Get) Error Recovery Control command failed" (scterc section)
		const auto re2= app_regex_re("/^((?:Error )?SMART WRITE LOG does not return COUNT and LBA_LOW register)$/mi");
		// "Read SCT Status failed: scsi error aborted command"
		// in front of "Read SCT Temperature History failed" and "SCT (Get) Error Recovery Control command failed"
		const auto re3= app_regex_re("/^(Read SCT Status failed: .*)$/mi");
		// "Unknown SCT Status format version 0, should be 2 or 3."
		const auto re4= app_regex_re("/^(Unknown SCT Status format version .*)$/mi");
		// "Read SCT Data Table failed: scsi error aborted command"
		const auto re5= app_regex_re("/^(Read SCT Data Table failed: .*)$/mi");
		// "Write SCT Data Table failed: Undefined error: 0"
		// in front of "Read SCT Temperature History failed"
		const auto re6= app_regex_re("/^(Write SCT Data Table failed: .*)$/mi");
		// "Unexpected SCT status 0x0000 (action_code=0, function_code=0)"
		// in front of "Read SCT Temperature History failed"
		const auto re7= app_regex_re("/^(Unexpected SCT status .*\\))$/mi");
		std::string match;

		if (app_regex_partial_match(re1, s, &match))
			app_regex_replace(re1, "", s);  // add extra newlines

		if (app_regex_partial_match(re2, s, &match))
			app_regex_replace(re2, "", s);  // add extra newlines

		if (app_regex_partial_match(re3, s, &match))
			app_regex_replace(re3, "", s);  // add extra newlines

		if (app_regex_partial_match(re4, s, &match))
			app_regex_replace(re4, "", s);  // add extra newlines

		if (app_regex_partial_match(re5, s, &match))
			app_regex_replace(re5, "", s);  // add extra newlines

		if (app_regex_partial_match(re6, s, &match))
			app_regex_replace(re6, "", s);  // add extra newlines

		if (app_regex_partial_match(re7, s, &match))
			app_regex_replace(re7, "", s);  // add extra newlines
	}


	// ------------------- Parsing

	// version info

	std::string version, version_full;
	if (!SmartctlVersionParser::parse_version_text(s, version, version_full)) {
		debug_out_warn("app", DBG_FUNC_MSG << "Cannot extract version information. Returning.\n");
		return hz::Unexpected(SmartctlParserError::NoVersion, "Cannot extract smartctl version information.");
	}

	{
		StorageProperty p;
		p.set_name("smartctl/version/_merged", _("Smartctl Version"));
		p.reported_value = version;
		p.value = p.reported_value;  // string-type value
		p.section = StoragePropertySection::Info;  // add to info section
		add_property(p);
	}
	{
		StorageProperty p;
		p.set_name("smartctl/version/_merged_full", _("Smartctl Version"));
		p.reported_value = version_full;
		p.value = p.reported_value;  // string-type value
		p.section = StoragePropertySection::Info;  // add to info section
		add_property(p);
	}

	if (!SmartctlVersionParser::check_format_supported(SmartctlOutputFormat::Text, version)) {
		debug_out_warn("app", DBG_FUNC_MSG << "Incompatible smartctl version. Returning.\n");
		return hz::Unexpected(SmartctlParserError::IncompatibleVersion, "Incompatible smartctl version.");
	}

	// Full text output
	{
		StorageProperty p;
		p.set_name("smartctl/output", "Smartctl Text Output");
		p.reported_value = smartctl_output;
		p.value = p.reported_value;  // string-type value
		p.show_in_ui = false;
		add_property(p);
	}


	// sections

	std::string::size_type section_start_pos = 0, section_end_pos = 0, tmp_pos = 0;
	bool status = false;  // true if at least one section was parsed

	// sections are started by
	// === START OF <NAME> SECTION ===
	while (section_start_pos != std::string::npos
			&& (section_start_pos = s.find("=== START", section_start_pos)) != std::string::npos) {

		tmp_pos = s.find('\n', section_start_pos);  // works with \r\n too. This may be npos if nothing follows the header.

		// trim is needed to remove potential \r in the end
		const std::string section_header = hz::string_trim_copy(s.substr(section_start_pos,
				(tmp_pos == std::string::npos ? tmp_pos : (tmp_pos - section_start_pos)) ));

		std::string section_body_str;
		if (tmp_pos != std::string::npos) {
			section_end_pos = s.find("=== START", tmp_pos);  // start of the next section
			section_body_str = hz::string_trim_copy(s.substr(tmp_pos,
					(section_end_pos == std::string::npos ? section_end_pos : section_end_pos - tmp_pos)));
		}
		status = parse_section(section_header, section_body_str).has_value() || status;
		section_start_pos = (tmp_pos == std::string::npos ? std::string::npos : section_end_pos);
	}

	if (!status) {
		debug_out_warn("app", DBG_FUNC_MSG << "No ATA sections could be parsed. Returning.\n");
		return hz::Unexpected(SmartctlParserError::NoSection, "No ATA sections could be parsed.");
	}

	return {};
}



// Parse the section part (with "=== .... ===" header) - info or data sections.
hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section(const std::string& header, const std::string& body)
{
	if (app_regex_partial_match("/START OF INFORMATION SECTION/mi", header)) {
		return parse_section_info(body);
	}

	if (app_regex_partial_match("/START OF READ SMART DATA SECTION/mi", header)) {
		return parse_section_data(body);
	}

	// These sections provide information about actions performed.
	// You may encounter this if e.g. executing "smartctl -a -s on".

	// example contents: "SMART Enabled.".
	if (app_regex_partial_match("/START OF READ SMART DATA SECTION/mi", header)) {
		return {};
	}

	// We don't parse this - it's parsed by the respective command issuer.
	if (app_regex_partial_match("/START OF ENABLE/DISABLE COMMANDS SECTION/mi", header)) {
		return {};
	}

	// This is printed when executing "-t long", etc. . Parsed by respective command issuer.
	if (app_regex_partial_match("/START OF OFFLINE IMMEDIATE AND SELF-TEST SECTION/mi", header)) {
		return {};
	}

	debug_out_warn("app", DBG_FUNC_MSG << "Unknown section encountered.\n");
	debug_out_dump("app", "---------------- Begin unknown section header dump ----------------\n");
	debug_out_dump("app", header << "\n");
	debug_out_dump("app", "----------------- End unknown section header dump -----------------\n");

	return hz::Unexpected(SmartctlParserError::UnknownSection, "Unknown section encountered.");
}




// ------------------------------------------------ INFO SECTION


hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_info(const std::string& body)
{
	this->set_data_section_info(body);

	const StoragePropertySection section = StoragePropertySection::Info;

	// split by lines.
	// e.g. Device Model:     ST3500630AS
	const auto re = app_regex_re("/^([^:]+):[ \\t]+(.*)$/i");  // MUST BE Ungreedy!

	std::vector<std::string> lines;
	hz::string_split(body, '\n', lines, false);
	std::string name, value, warning_msg;
	bool expecting_warning_lines = false;

// 	while (re.FindAndConsume(&input, &name, &value)) {
	for (auto line : lines) {
		hz::string_trim(line);

		if (expecting_warning_lines) {
			if (!line.empty()) {
				warning_msg += "\n" + line;
			} else {
				expecting_warning_lines = false;
				StorageProperty p;
				p.section = section;
				p.set_name("_text_only/info_warning", _("Warning"));
				p.reported_value = warning_msg;
				p.value = p.reported_value;  // string-type value
				add_property(p);
				warning_msg.clear();
			}
			continue;
		}

		if (line.empty()) {
			continue;  // empty lines are part of Info section
		}

		// Sometimes, we get this in the middle of Info section (separated by double newlines):
/*
==> WARNING: A firmware update for this drive may be available,
see the following Seagate web pages:
http://knowledge.seagate.com/articles/en_US/FAQ/207931en
http://knowledge.seagate.com/articles/en_US/FAQ/213891en
*/
		if (app_regex_partial_match("/^==> WARNING: /mi", line)) {
			app_regex_replace("^==> WARNING: ", "", line);
			warning_msg = hz::string_trim_copy(line);
			expecting_warning_lines = true;
			continue;
		}

		// This is not an ordinary name / value pair, so filter it out (we don't need it anyway).
		// Usually this happens when smart is unsupported or disabled.
		if (app_regex_partial_match("/mandatory SMART command failed/mi", line)) {
			continue;
		}
		// --get=all may cause these, ignore.
				// "Unexpected SCT status 0x0010 (action_code=4, function_code=2)"
		if (app_regex_partial_match("/^Unexpected SCT status/mi", line)
				// "Write SCT (Get) XXX Error Recovery Control Command failed: scsi error aborted command"
				|| app_regex_partial_match("/^Write SCT \\(Get\\) XXX Error Recovery Control Command failed/mi", line)
				// "Write SCT (Get) Feature Control Command failed: scsi error aborted command"
				|| app_regex_partial_match("/^Write SCT \\(Get\\) Feature Control Command failed/mi", line)
				// "Read SCT Status failed: scsi error aborted command"
				|| app_regex_partial_match("/^Read SCT Status failed/mi", line)
				// "Read SMART Data failed: Input/output error"  (just ignore this, the rest of the data seems fine)
				|| app_regex_partial_match("/^Read SMART Data failed/mi", line)
				// "Unknown SCT Status format version 0, should be 2 or 3."
				|| app_regex_partial_match("/^Unknown SCT Status format version/mi", line)
				// "Read SMART Thresholds failed: scsi error aborted command"
				|| app_regex_partial_match("/^Read SMART Thresholds failed/mi", line)
				// "                  Enabled status cached by OS, trying SMART RETURN STATUS cmd."
				|| app_regex_partial_match("/Enabled status cached by OS, trying SMART RETURN STATUS cmd/mi", line)
				|| app_regex_partial_match("/^>> Terminate command early due to bad response to IEC mode page/mi", line)  // on a flash drive
				// "scsiModePageOffset: response length too short, resp_len=4 offset=4 bd_len=0"
				|| app_regex_partial_match("/^scsiModePageOffset: .+/mi", line)  // on a flash drive
		   ) {
			continue;
		}

		if (app_regex_full_match(re, line, {&name, &value})) {
			hz::string_trim(name);
			hz::string_trim(value);

			StorageProperty p;
			p.section = section;
			p.set_name(name, name, name);
			p.reported_value = value;

			auto result = parse_section_info_property(p);  // set type and the typed value. may change generic_name too.
			if (!result.has_value()) {  // internal error
				return result;
			}
			add_property(p);

		} else {
			debug_out_warn("app", DBG_FUNC_MSG << "Unknown Info line encountered.\n");
			debug_out_dump("app", "---------------- Begin unknown Info line ----------------\n");
			debug_out_dump("app", line << "\n");
			debug_out_dump("app", "----------------- End unknown Info line -----------------\n");
		}
	}

	return {};
}



// Parse a component (one line) of the info section
hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_info_property(StorageProperty& p)
{
	// ---- Info
	if (p.section != StoragePropertySection::Info) {
		debug_out_error("app", DBG_FUNC_MSG << "Called with non-info section!\n");
		return hz::Unexpected(SmartctlParserError::InternalError, "Internal parser error.");
	}


	if (app_regex_partial_match("/^Model Family$/mi", p.reported_name)) {
		p.set_name("model_family", "Model Family", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^(?:Device Model|Device|Product)$/mi", p.reported_name)) {  // "Device" and "Product" are from scsi/usb
		p.set_name("model_name", "Device Model", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Vendor$/mi", p.reported_name)) {  // From scsi/usb
		p.set_name("vendor", "Vendor", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Revision$/mi", p.reported_name)) {  // From scsi/usb
		p.set_name("revision", "Revision", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Device type$/mi", p.reported_name)) {  // From scsi/usb
		p.set_name("device_type/name", "Device Type", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Compliance$/mi", p.reported_name)) {  // From scsi/usb
		p.set_name("scsi_version", "Compliance", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Serial Number$/mi", p.reported_name)) {
		p.set_name("serial_number", "Serial Number", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^LU WWN Device Id$/mi", p.reported_name)) {
		p.set_name("wwn/_merged", "World Wide Name", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Add. Product Id$/mi", p.reported_name)) {
		p.set_name("ata_additional_product_id", "Additional Product ID", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Firmware Version$/mi", p.reported_name)) {
		p.set_name("firmware_version", "Firmware Version", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^User Capacity$/mi", p.reported_name)) {
		p.set_name("user_capacity/bytes", "Capacity", p.reported_name);
		int64_t v = 0;
		p.readable_value = SmartctlTextParserHelper::parse_byte_size(p.reported_value, v, true);
		if (p.readable_value.empty()) {
			p.readable_value = "[unknown]";
		} else {
			p.value = v;  // integer-type value
		}

	} else if (app_regex_partial_match("/^Sector Sizes$/mi", p.reported_name)) {
		p.set_name("physical_block_size/_and/logical_block_size", "Sector Sizes", p.reported_name);
		// This contains 2 values (phys/logical, if they're different)
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Sector Size$/mi", p.reported_name)) {
		p.set_name("physical_block_size/_and/logical_block_size", "Sector Size", p.reported_name);
		// This contains a single value (if it's not 512)
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Logical block size$/mi", p.reported_name)) {  // from scsi/usb
		p.set_name("logical_block_size", "Logical Block Size", p.reported_name);
		// "512 bytes"
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Rotation Rate$/mi", p.reported_name)) {
		p.set_name("rotation_rate", "Rotation Rate", p.reported_name);
		p.value = hz::string_to_number_nolocale<int64_t>(p.reported_value, false);

	} else if (app_regex_partial_match("/^Form Factor$/mi", p.reported_name)) {
		p.set_name("form_factor/name", "Form Factor", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Device is$/mi", p.reported_name)) {
		p.set_name("in_smartctl_database", "In Smartctl Database", p.reported_name);
		p.value = (!app_regex_partial_match("/Not in /mi", p.reported_value));  // bool-type value

	} else if (app_regex_partial_match("/^ATA Version is$/mi", p.reported_name)) {
		p.set_name("ata_version/string", "ATA Version", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^ATA Standard is$/mi", p.reported_name)) {  // old, not present in smartctl 7.2
		p.set_name("ata_version/string", "ATA Standard", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^SATA Version is$/mi", p.reported_name)) {
		p.set_name("sata_version/string", "SATA Version", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Local Time is$/mi", p.reported_name)) {
		p.set_name("local_time/asctime", "Scanned on", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^SMART support is$/mi", p.reported_name)) {
		// There are two different properties with this name - supported and enabled.
		// Don't put complete messages here - they change across smartctl versions.

		if (app_regex_partial_match("/Available - device has/mi", p.reported_value)) {
			p.set_name("smart_support/available", "SMART Supported", p.reported_name);
			p.value = true;

		} else if (app_regex_partial_match("/Enabled/mi", p.reported_value)) {
			p.set_name("smart_support/enabled", "SMART Enabled", p.reported_name);
			p.value = true;

		} else if (app_regex_partial_match("/Disabled/mi", p.reported_value)) {
			p.set_name("smart_support/enabled", "SMART Enabled", p.reported_name);
			p.value = false;

		} else if (app_regex_partial_match("/Unavailable/mi", p.reported_value)) {
			p.set_name("smart_support/available", "SMART Supported", p.reported_name);
			p.value = false;

		// this should be the last - when ambiguous state is detected, usually smartctl
		// retries with other methods and prints one of the above.
		} else if (app_regex_partial_match("/Ambiguous/mi", p.reported_value)) {
			p.set_name("smart_support/available", "SMART Supported", p.reported_name);
			p.value = true;  // let's be optimistic - just hope that it doesn't hurt.
		}

	// "-g all" stuff
	} else if (app_regex_partial_match("/^AAM feature is$/mi", p.reported_name)) {
		p.set_name("ata_aam/enabled", "AAM Feature", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^AAM level is$/mi", p.reported_name)) {
		p.set_name("ata_aam/level", "AAM Level", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^APM feature is$/mi", p.reported_name)) {
		p.set_name("ata_apm/enabled", "APM Feature", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^APM level is$/mi", p.reported_name)) {
		p.set_name("ata_apm/level", "APM Level", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Rd look-ahead is$/mi", p.reported_name)) {
		p.set_name("read_lookahead/enabled", "Read Look-Ahead", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Write cache is$/mi", p.reported_name)) {
		p.set_name("write_cache/enabled", "Write Cache", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Wt Cache Reorder$/mi", p.reported_name)) {
		p.set_name("_text_only/write_cache_reorder", "Write Cache Reorder", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^DSN feature is$/mi", p.reported_name)) {
		p.set_name("ata_dsn/enabled", "DSN Feature", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^Power mode (?:was|is)$/mi", p.reported_name)) {
		p.set_name("_text_only/power_mode", "Power Mode", p.reported_name);
		p.value = p.reported_value;  // string-type value

	} else if (app_regex_partial_match("/^ATA Security is$/mi", p.reported_name)) {
		p.set_name("ata_security/string", "ATA Security", p.reported_name);
		p.value = p.reported_value;  // string-type value

	// These are some debug warnings from smartctl on usb flash drives
	} else if (app_regex_partial_match("/^scsiMode/mi", p.reported_name)) {
		p.show_in_ui = false;

	} else {
		debug_out_warn("app", DBG_FUNC_MSG << "Unknown property \"" << p.reported_name << "\"\n");
		// this is not an error, just unknown attribute. treat it as string.
		// Don't highlight it with warning, it may just be a new smartctl feature.
		p.value = p.reported_value;  // string-type value
	}

	return {};
}




// ------------------------------------------------ DATA SECTION


// Parse the Data section (without "===" header)
hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data(const std::string& body)
{
	this->set_data_section_data(body);

	// perform any2unix
// 	std::string s = hz::string_any_to_unix_copy(body);

	std::vector<std::string> split_subsections;
	// subsections are separated by double newlines, except:
	// - "error log" subsection, which contains double-newline-separated blocks.
	// - "scttemp" subsection, which has 3 blocks.
	hz::string_split(body, "\n\n", split_subsections, true);

	bool status = false;  // at least one subsection was parsed


	std::vector<std::string> subsections;

	// merge "single " parts. For error log, each part begins with a double-space or "Error nn".
	// For scttemp, parts begin with
	// "SCT Temperature History Version" or
	// "Index    " or
	// "Read SCT Temperature History failed".
	for (auto sub : split_subsections) {
		hz::string_trim(sub, "\t\n\r");  // don't trim space
		if (app_regex_partial_match("^  ", sub)
				|| app_regex_partial_match("^Error [0-9]+", sub)
				|| app_regex_partial_match("^SCT Temperature History Version", sub)
				|| app_regex_partial_match("^Index[ \t]+", sub)
				|| app_regex_partial_match("^Read SCT Temperature History failed", sub) ) {
			if (!subsections.empty()) {
				subsections.back() += "\n\n" + sub;  // append to previous part
			} else {
				debug_out_warn("app", DBG_FUNC_MSG << "Error Log's Error block, or SCT Temperature History, or SCT Index found without any data subsections present.\n");
			}
		} else {  // not an Error block, process as usual
			subsections.push_back(sub);
		}
	}


	// parse each subsection
	for (auto sub : subsections) {
		hz::string_trim(sub);
		if (sub.empty())
			continue;

		if (app_regex_partial_match("/^SMART overall-health self-assessment/mi", sub)) {
			status = parse_section_data_subsection_health(sub).has_value() || status;

		} else if (app_regex_partial_match("/^General SMART Values/mi", sub)) {
			status = parse_section_data_subsection_capabilities(sub).has_value() || status;

		} else if (app_regex_partial_match("/^SMART Attributes Data Structure/mi", sub)) {
			status = parse_section_data_subsection_attributes(sub).has_value() || status;

		} else if (app_regex_partial_match("/^General Purpose Log Directory Version/mi", sub)  // -l directory
				|| app_regex_partial_match("/^General Purpose Log Directory not supported/mi", sub)
				|| app_regex_partial_match("/^General Purpose Logging \\(GPL\\) feature set supported/mi", sub)
				|| app_regex_partial_match("/^Read GP Log Directory failed/mi", sub)
				|| app_regex_partial_match("/^Log Directories not read due to '-F nologdir' option/mi", sub)
				|| app_regex_partial_match("/^Read SMART Log Directory failed/mi", sub)
				|| app_regex_partial_match("/^SMART Log Directory Version/mi", sub) ) {  // old smartctl
			status = parse_section_data_subsection_directory_log(sub).has_value() || status;

		} else if (app_regex_partial_match("/^SMART Error Log Version/mi", sub)  // -l error
				|| app_regex_partial_match("/^SMART Extended Comprehensive Error Log Version/mi", sub)  // -l xerror
				|| app_regex_partial_match("/^Warning: device does not support Error Logging/mi", sub)  // -l error
				|| app_regex_partial_match("/^SMART Error Log not supported/mi", sub)  // -l error
				|| app_regex_partial_match("/^Read SMART Error Log failed/mi", sub) ) {  // -l error
			status = parse_section_data_subsection_error_log(sub).has_value() || status;

		} else if (app_regex_partial_match("/^SMART Extended Comprehensive Error Log \\(GP Log 0x03\\) not supported/mi", sub)  // -l xerror
				|| app_regex_partial_match("/^SMART Extended Comprehensive Error Log size (.*) not supported/mi", sub)
				|| app_regex_partial_match("/^Read SMART Extended Comprehensive Error Log failed/mi", sub) ) {  // -l xerror
			// These are printed with "-l xerror,error" if falling back to "error". They're in their own sections, ignore them.
			// We don't support showing these messages.
			status = false;

		} else if (app_regex_partial_match("/^SMART Self-test log/mi", sub)  // -l selftest
				|| app_regex_partial_match("/^SMART Extended Self-test Log Version/mi", sub)  // -l xselftest
				|| app_regex_partial_match("/^Warning: device does not support Self Test Logging/mi", sub)  // -l selftest
				|| app_regex_partial_match("/^Read SMART Self-test Log failed/mi", sub)  // -l selftest
				|| app_regex_partial_match("/^SMART Self-test Log not supported/mi", sub)) {  // -l selftest
			status = parse_section_data_subsection_selftest_log(sub).has_value() || status;

		} else if (app_regex_partial_match("/^SMART Extended Self-test Log \\(GP Log 0x07\\) not supported/mi", sub)  // -l xselftest
				|| app_regex_partial_match("/^SMART Extended Self-test Log size [0-9-]+ not supported/mi", sub)  // -l xselftest
				|| app_regex_partial_match("/^Read SMART Extended Self-test Log failed/mi", sub) ) {  // -l xselftest
			// These are printed with "-l xselftest,selftest" if falling back to "selftest". They're in their own sections, ignore them.
			// We don't support showing these messages.
			status = false;

		} else if (app_regex_partial_match("/^SMART Selective self-test log data structure/mi", sub)
				|| app_regex_partial_match("/^Device does not support Selective Self Tests\\/Logging/mi", sub)
				|| app_regex_partial_match("/^Selective Self-tests\\/Logging not supported/mi", sub)
				|| app_regex_partial_match("/^Read SMART Selective Self-test Log failed/mi", sub) ) {
			status = parse_section_data_subsection_selective_selftest_log(sub).has_value() || status;

		} else if (app_regex_partial_match("/^SCT Status Version/mi", sub)
				// "SCT Commands not supported"
				// "SCT Commands not supported if ATA Security is LOCKED"
				// "Error unknown SCT Temperature History Format Version (3), should be 2."
				// "Another SCT command is executing, abort Read Data Table"
				|| app_regex_partial_match("/^SCT Commands not supported/mi", sub)
				|| app_regex_partial_match("/^SCT Data Table command not supported/mi", sub)
				|| app_regex_partial_match("/^Error unknown SCT Temperature History Format Version/mi", sub)
				|| app_regex_partial_match("/^Another SCT command is executing, abort Read Data Table/mi", sub)
				|| app_regex_partial_match("/^Warning: device does not support SCT Commands/mi", sub) ) {  // old smartctl
			status = parse_section_data_subsection_scttemp_log(sub).has_value() || status;

		} else if (app_regex_partial_match("/^SCT Error Recovery Control/mi", sub)
				// Can be the same "SCT Commands not supported" as scttemp.
				// "Another SCT command is executing, abort Error Recovery Control"
				|| app_regex_partial_match("/^SCT Error Recovery Control command not supported/mi", sub)
				|| app_regex_partial_match("/^SCT \\(Get\\) Error Recovery Control command failed/mi", sub)
				|| app_regex_partial_match("/^Another SCT command is executing, abort Error Recovery Control/mi", sub)
				|| app_regex_partial_match("/^Warning: device does not support SCT \\(Get\\) Error Recovery Control/mi", sub) ) {  // old smartctl
			status = parse_section_data_subsection_scterc_log(sub).has_value() || status;

		} else if (app_regex_partial_match("/^Device Statistics \\([^)]+\\)$/mi", sub)  // -l devstat
				|| app_regex_partial_match("/^Device Statistics \\([^)]+\\) not supported/mi", sub)
				|| app_regex_partial_match("/^Read Device Statistics page (?:.+) failed/mi", sub) ) {
			status = parse_section_data_subsection_devstat(sub).has_value() || status;

		// "Device Statistics (GP Log 0x04) supported pages"
		} else if (app_regex_partial_match("/^Device Statistics \\([^)]+\\) supported pages/mi", sub) ) {  // not sure where it came from
			// We don't support this section.
			status = false;

		} else if (app_regex_partial_match("/^SATA Phy Event Counters/mi", sub)  // -l sataphy
				|| app_regex_partial_match("/^SATA Phy Event Counters \\(GP Log 0x11\\) not supported/mi", sub)
				|| app_regex_partial_match("/^SATA Phy Event Counters with [0-9-]+ sectors not supported/mi", sub)
				|| app_regex_partial_match("/^Read SATA Phy Event Counters failed/mi", sub) ) {
			status = parse_section_data_subsection_sataphy(sub).has_value() || status;

		} else {
			debug_out_warn("app", DBG_FUNC_MSG << "Unknown Data subsection encountered.\n");
			debug_out_dump("app", "---------------- Begin unknown section dump ----------------\n");
			debug_out_dump("app", sub << "\n");
			debug_out_dump("app", "----------------- End unknown section dump -----------------\n");
		}
	}

	return hz::Unexpected(SmartctlParserError::NoSubsectionsParsed, "No subsections could be parsed.");
}




// -------------------- Health

hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_health(const std::string& sub)
{
	// Health section data (--info and --get=all):
/*
Model Family:     Hitachi/HGST Travelstar 5K750
Device Model:     Hitachi HTS547550A9E384
Firmware Version: JE3OA40J
User Capacity:    500,107,862,016 bytes [500 GB]
Sector Sizes:     512 bytes logical, 4096 bytes physical
Rotation Rate:    5400 rpm
Form Factor:      2.5 inches
Device is:        In smartctl database [for details use: -P show]
*/

	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::OverallHealth;

	std::string name, value;
	if (app_regex_partial_match("/^([^:\\n]+):[ \\t]*(.*)$/mi", sub, {&name, &value})) {
		hz::string_trim(name);
		hz::string_trim(value);

		// only one attribute in this section
		if (app_regex_partial_match("/SMART overall-health self-assessment/mi", name)) {
			pt.set_name(name, "smart_status/passed", "Overall Health Self-Assessment Test");
			pt.reported_value = value;
			pt.value = (pt.reported_value == "PASSED");  // bool
			pt.readable_value = (pt.get_value<bool>() ? "PASSED" : "FAILED");

			add_property(pt);
		}

		return {};
	}

	return hz::Unexpected(SmartctlParserError::DataError, "Empty health subsection.");
}




// -------------------- Capabilities

hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_capabilities(const std::string& sub_initial)
{
	// Capabilities section data:
/*
General SMART Values:
Offline data collection status:  (0x82)	Offline data collection activity
					was completed without error.
					Auto Offline Data Collection: Enabled.
Self-test execution status:      (   0)	The previous self-test routine completed
					without error or no self-test has ever
					been run.
Total time to complete Offline
data collection: 		(   45) seconds.
Offline data collection
capabilities: 			 (0x5b) SMART execute Offline immediate.
					Auto Offline data collection on/off support.
					Suspend Offline collection upon new
					command.
					Offline surface scan supported.
					Self-test supported.
					No Conveyance Self-test supported.
					Selective Self-test supported.
SMART capabilities:            (0x0003)	Saves SMART data before entering
					power-saving mode.
					Supports SMART auto save timer.
Error logging capability:        (0x01)	Error logging supported.
					General Purpose Logging supported.
Short self-test routine
recommended polling time: 	 (   2) minutes.
Extended self-test routine
recommended polling time: 	 ( 152) minutes.
SCT capabilities: 	       (0x003d)	SCT Status supported.
					SCT Error Recovery Control supported.
					SCT Feature Control supported.
					SCT Data Table supported.
*/

	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::Capabilities;

	std::string sub = sub_initial;

	// Fix some bugs in smartctl output (pre-5.39-final versions):
	// There is a stale newline in "is in a Vendor Specific state\n.\n" and
	// "is in a Reserved state\n.\n".
// 	app_regex_replace("/\\n\\.$/mi", ".", &sub);
	app_regex_replace("/(is in a Vendor Specific state)\\n\\.$/mi", "\\1.", sub);
	app_regex_replace("/(is in a Reserved state)\\n\\.$/mi", "\\1.", sub);


	// split to lines and merge them into blocks
	std::vector<std::string> lines, blocks;
	hz::string_split(sub, '\n', lines, true);
	bool partial = false;

	for(auto line : lines) {
		if (line.empty() || app_regex_partial_match("/General SMART Values/mi", line))  // skip the non-informative lines
			continue;
		line += "\n";  // avoid joining lines without separator. this will get stripped anyway.

		if (line.find_first_of(" \t") != 0 && !partial) {  // new blocks don't start with whitespace
			blocks.emplace_back();  // new block
			blocks.back() += line;
			if (line.find(':') == std::string::npos)
				partial = true;  // if the name spans several lines (they all start with non-whitespace)
			continue;
		}

		if (partial && line.find(':') != std::string::npos)
			partial = false;

		if (blocks.empty()) {
			debug_out_error("app", DBG_FUNC_MSG << "Non-block related line found!\n");
			blocks.emplace_back();  // avoid segfault
		}
		blocks.back() += line;
	}


	// parse each block.
	// [\s\S] is equivalent to dot matching newlines.
	const auto re = app_regex_re(R"(/([^:]*):\s*\(([^)]+)\)\s*([\s\S]*)/m)");

	bool cap_found = false;  // found at least one capability

	for(std::size_t i = 0; i < blocks.size(); ++i) {
		const std::string block = hz::string_trim_copy(blocks[i]);

		std::string name_orig, numvalue_orig, strvalue_orig;

		if (!app_regex_full_match(re, block, {&name_orig, &numvalue_orig, &strvalue_orig})) {
			debug_out_error("app", DBG_FUNC_MSG << "Block "
					<< i << " cannot be parsed.\n");
			debug_out_dump("app", "---------------- Begin unparsable block dump ----------------\n");
			debug_out_dump("app", block << "\n");
			debug_out_dump("app", "----------------- End unparsable block dump -----------------\n");
			continue;
		}

		// flatten:
		const std::string name = hz::string_trim_copy(hz::string_remove_adjacent_duplicates_copy(
				hz::string_replace_chars_copy(name_orig, "\t\n", ' '), ' '));

		const std::string strvalue = hz::string_trim_copy(hz::string_remove_adjacent_duplicates_copy(
				hz::string_replace_chars_copy(strvalue_orig, "\t\n", ' '), ' '));

		int64_t numvalue = -1;
		if (!hz::string_is_numeric_nolocale<int64_t>(hz::string_trim_copy(numvalue_orig), numvalue, false)) {  // this will autodetect number base.
			debug_out_warn("app", DBG_FUNC_MSG
					<< "Numeric value: \"" << numvalue_orig << "\" cannot be parsed as number.\n");
		}

		// 		debug_out_dump("app", "name: \"" << name << "\"\n\tnumvalue: \"" << numvalue
		// 				<< "\"\n\tstrvalue: \"" << strvalue << "\"\n\n");


		// Time length properties
		if (hz::string_erase_right_copy(strvalue, ".") == "minutes"
				|| hz::string_erase_right_copy(strvalue, ".") == "seconds") {

			// const int numvalue_unmod = numvalue;

			if (hz::string_erase_right_copy(strvalue, ".") == "minutes")
				numvalue *= 60;  // convert to seconds

			// add as a time property
			StorageProperty p(pt);
			p.set_name(name, name, name);
			// well, not really as reported, but still...
			p.reported_value.append(numvalue_orig).append(" | ").append(strvalue_orig);
			p.value = std::chrono::seconds(numvalue);  // always in seconds

			// Set some generic names on the recognized ones
			if (parse_section_data_internal_capabilities(p).has_value()) {
				add_property(p);
				cap_found = true;
			}


		// AtaStorageCapability properties (capabilities are flag lists)
		} else {

			StorageProperty p(pt);
			p.set_name(name, name, name);
			// well, not really as reported, but still...
			p.reported_value.append(numvalue_orig).append(" | ").append(strvalue_orig);

			AtaStorageTextCapability cap;
			cap.reported_flag_value = numvalue_orig;
			cap.flag_value = static_cast<uint16_t>(numvalue);  // full flag value
			cap.reported_strvalue = strvalue_orig;

			// split capability lines into a vector. every flag sentence ends with "."
			hz::string_split(strvalue, '.', cap.strvalues, true);
			for (auto&& v : cap.strvalues) {
				hz::string_trim(v);
			}

			p.value = cap;  // Capability-type value

			// find some special capabilities we're interested in and add them. p is unmodified.
			if (parse_section_data_internal_capabilities(p)) {
				add_property(p);
				cap_found = true;
			}
		}

	}

	if (!cap_found)
		return hz::Unexpected(SmartctlParserError::DataError, "No capabilities found in Capabilities section.");

	return {};
}





// Check the capabilities for internal properties we can use.
hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_internal_capabilities(StorageProperty& cap_prop)
{
	// Some special capabilities we're interested in.

	// Note: Smartctl gradually changed spelling Off-line to Offline in some messages.
	// Also, some capitalization was changed (so the regexps are caseless).

	// "Offline data collection not supported." (at all) - we don't need to check this,
	// because we look for immediate/automatic anyway.

	// "was never started", "was completed without error", "is in progress",
	// "was suspended by an interrupting command from host", etc.
	const auto re_offline_status = app_regex_re("/^(Off-?line data collection) activity (?:is|was) (.*)$/mi");
	// "Enabled", "Disabled". May not show up on older smartctl (< 5.1.10), so no way of knowing there.
	const auto re_offline_enabled = app_regex_re("/^(Auto Off-?line Data Collection):[ \\t]*(.*)$/mi");
	const auto re_offline_immediate = app_regex_re("/^(SMART execute Off-?line immediate)$/mi");
	// "No Auto Offline data collection support.", "Auto Offline data collection on/off support.".
	const auto re_offline_auto = app_regex_re("/^(No |)(Auto Off-?line data collection (?:on\\/off )?support)$/mi");
	// Same as above (smartctl <= 5.1-18). "No Automatic timer ON/OFF support."
	const auto re_offline_auto2 = app_regex_re("/^(No |)(Automatic timer ON\\/OFF support)$/mi");
	const auto re_offline_suspend = app_regex_re("/^(?:Suspend|Abort) (Off-?line collection upon new command)$/mi");
	const auto re_offline_surface = app_regex_re("/^(No |)(Off-?line surface scan supported)$/mi");

	const auto re_selftest_support = app_regex_re("/^(No |)(Self-test supported)$/mi");
	const auto re_conv_selftest_support = app_regex_re("/^(No |)(Conveyance Self-test supported)$/mi");
	const auto re_selective_selftest_support = app_regex_re("/^(No |)(Selective Self-test supported)$/mi");

	const auto re_sct_status = app_regex_re("/^(SCT Status supported)$/mi");
	const auto re_sct_control = app_regex_re("/^(SCT Feature Control supported)$/mi");  // means can change logging interval
	const auto re_sct_data = app_regex_re("/^(SCT Data Table supported)$/mi");

	// these are matched on name
	const auto re_offline_status_group = app_regex_re("/^(Off-?line data collection status)/mi");
	const auto re_offline_time = app_regex_re("/^(Total time to complete Off-?line data collection)/mi");
	const auto re_offline_cap_group = app_regex_re("/^(Off-?line data collection capabilities)/mi");
	const auto re_smart_cap_group = app_regex_re("/^(SMART capabilities)/mi");
	const auto re_error_log_cap_group = app_regex_re("/^(Error logging capability)/mi");
	const auto re_sct_cap_group = app_regex_re("/^(SCT capabilities)/mi");
	const auto re_selftest_status = app_regex_re("/^Self-test execution status/mi");
	const auto re_selftest_short_time = app_regex_re("/^(Short self-test routine recommended polling time)/mi");
	const auto re_selftest_long_time = app_regex_re("/^(Extended self-test routine recommended polling time)/mi");
	const auto re_conv_selftest_time = app_regex_re("/^(Conveyance self-test routine recommended polling time)/mi");

	if (cap_prop.section != StoragePropertySection::Capabilities) {
		debug_out_error("app", DBG_FUNC_MSG << "Non-capability property passed.\n");
		return hz::Unexpected(SmartctlParserError::DataError, "Non-capability property passed.");
	}


	// Name the capability groups for easy matching when setting descriptions
	if (cap_prop.is_value_type<AtaStorageTextCapability>()) {
		if (app_regex_partial_match(re_offline_status_group, cap_prop.reported_name)) {
			cap_prop.generic_name = "ata_smart_data/offline_data_collection/status/_group";

		} else if (app_regex_partial_match(re_offline_cap_group, cap_prop.reported_name)) {
			cap_prop.generic_name = "ata_smart_data/offline_data_collection/_group";

		} else if (app_regex_partial_match(re_smart_cap_group, cap_prop.reported_name)) {
			cap_prop.generic_name = "ata_smart_data/capabilities/_group";

		} else if (app_regex_partial_match(re_error_log_cap_group, cap_prop.reported_name)) {
			cap_prop.generic_name = "ata_smart_data/capabilities/error_logging_supported/_group";

		} else if (app_regex_partial_match(re_sct_cap_group, cap_prop.reported_name)) {
			cap_prop.generic_name = "ata_sct_capabilities/_group";

		} else if (app_regex_partial_match(re_selftest_status, cap_prop.reported_name)) {
			cap_prop.generic_name = "ata_smart_data/self_test/status/_group";
		}
	}


	// Last self-test status
	if (app_regex_partial_match(re_selftest_status, cap_prop.reported_name)) {
		// The last self-test status. break up into pieces.

		StorageProperty p;
//		p.section = AtaStoragePropertySection::Internal;
		p.section = StoragePropertySection::Capabilities;
		p.set_name("ata_smart_data/self_test/status/_merged", _("Self-test execution status"));

		AtaStorageSelftestEntry sse;
		sse.test_num = 0;
		sse.remaining_percent = -1;  // unknown or n/a

		// check for lines in capability vector
		for (const auto& sv : cap_prop.get_value<AtaStorageTextCapability>().strvalues) {
			std::string value;

			if (app_regex_partial_match("/^([0-9]+)% of test remaining/mi", sv, &value)) {
				int8_t v = 0;
				if (hz::string_is_numeric_nolocale(value, v))
					sse.remaining_percent = v;

			} else if (app_regex_partial_match("/^(The previous self-test routine completed without error or no .*)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::CompletedNoError;

			} else if (app_regex_partial_match("/^(The self-test routine was aborted by the host)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::AbortedByHost;

			} else if (app_regex_partial_match("/^(The self-test routine was interrupted by the host with a hard.*)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::Interrupted;

			} else if (app_regex_partial_match("/^(A fatal error or unknown test error occurred while the device was executing its .*)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::FatalOrUnknown;

			} else if (app_regex_partial_match("/^(The previous self-test completed having a test element that failed and the test element that failed is not known)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::ComplUnknownFailure;

			} else if (app_regex_partial_match("/^(The previous self-test completed having the electrical element of the test failed)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::ComplElectricalFailure;

			} else if (app_regex_partial_match("/^(The previous self-test completed having the servo .*)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::ComplServoFailure;

			} else if (app_regex_partial_match("/^(The previous self-test completed having the read element of the test failed)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::ComplReadFailure;

			} else if (app_regex_partial_match("/^(The previous self-test completed having a test element that failed and the device is suspected of having handling damage)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::ComplHandlingDamage;

			// samsung bug (?), as per smartctl sources.
			} else if (app_regex_partial_match("/^(The previous self-test routine completed with unknown result or self-test .*)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::ComplUnknownFailure;  // we'll use this again (correct?)

			} else if (app_regex_partial_match("/^(Self-test routine in progress)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::InProgress;

			} else if (app_regex_partial_match("/^(Reserved)/mi", sv, &value)) {
				sse.status_str = value;
				sse.status = AtaStorageSelftestEntry::Status::Reserved;
			}
		}

		p.value = sse;  // AtaStorageSelftestEntry-type value

		add_property(p);

		return {};
	}


	// Check the time-related ones first.
	// Note: We only modify the existing property here!
	// Section is unmodified.
	if (cap_prop.is_value_type<std::chrono::seconds>()) {

		if (app_regex_partial_match(re_offline_time, cap_prop.reported_name)) {
			cap_prop.generic_name = "ata_smart_data/offline_data_collection/completion_seconds";

		} else if (app_regex_partial_match(re_selftest_short_time, cap_prop.reported_name)) {
			cap_prop.generic_name = "ata_smart_data/self_test/polling_minutes/short";

		} else if (app_regex_partial_match(re_selftest_long_time, cap_prop.reported_name)) {
			cap_prop.generic_name = "ata_smart_data/self_test/polling_minutes/extended";

		} else if (app_regex_partial_match(re_conv_selftest_time, cap_prop.reported_name)) {
			cap_prop.generic_name = "ata_smart_data/self_test/polling_minutes/conveyance";
		}

		return {};
	}


	// Extract subcapabilities from capability vectors and assign to "internal" section.
	if (cap_prop.is_value_type<AtaStorageTextCapability>()) {

		// check for lines in capability vector
		for (const auto& sv : cap_prop.get_value<AtaStorageTextCapability>().strvalues) {

			// debug_out_dump("app", "Looking for internal capability in: \"" << sv << "\"\n");

			StorageProperty p;
//			p.section = AtaStoragePropertySection::Internal;
			p.section = StoragePropertySection::Capabilities;
			// Note: We don't set reported_value on internal properties.

			std::string name, value;

			if (app_regex_partial_match(re_offline_status, sv, {&name, &value})) {
				p.set_name("ata_smart_data/offline_data_collection/status/string", name, name);
				p.value = hz::string_trim_copy(value);  // string-type value

			} else if (app_regex_partial_match(re_offline_enabled, sv, {&name, &value})) {
				p.set_name("ata_smart_data/offline_data_collection/status/value/_parsed", name, name);
				p.value = (hz::string_trim_copy(value) == "Enabled");  // bool

			} else if (app_regex_partial_match(re_offline_immediate, sv, &name)) {
				p.set_name("ata_smart_data/capabilities/exec_offline_immediate_supported", name, name);
				p.value = true;  // bool

			} else if (app_regex_partial_match(re_offline_auto, sv, {&value, &name})
					|| app_regex_partial_match(re_offline_auto2, sv, {&value, &name})) {
				p.set_name("_text_only/aodc_support", "Automatic Offline Data Collection toggle support", name);
				p.value = (hz::string_trim_copy(value) != "No");  // bool

			} else if (app_regex_partial_match(re_offline_suspend, sv, {&value, &name})) {
				p.set_name("ata_smart_data/capabilities/offline_is_aborted_upon_new_cmd", "Offline Data Collection suspends upon new command", name);
				p.value = (hz::string_trim_copy(value) == "Suspend");  // bool

			} else if (app_regex_partial_match(re_offline_surface, sv, {&value, &name})) {
				p.set_name("ata_smart_data/capabilities/offline_surface_scan_supported", name, name);
				p.value = (hz::string_trim_copy(value) != "No");  // bool


			} else if (app_regex_partial_match(re_selftest_support, sv, {&value, &name})) {
				p.set_name("ata_smart_data/capabilities/self_tests_supported", name, name);
				p.value = (hz::string_trim_copy(value) != "No");  // bool

			} else if (app_regex_partial_match(re_conv_selftest_support, sv, {&value, &name})) {
				p.set_name("ata_smart_data/capabilities/conveyance_self_test_supported", name, name);
				p.value = (hz::string_trim_copy(value) != "No");  // bool

			} else if (app_regex_partial_match(re_selective_selftest_support, sv, {&value, &name})) {
				p.set_name("ata_smart_data/capabilities/selective_self_test_supported", name, name);
				p.value = (hz::string_trim_copy(value) != "No");  // bool


			} else if (app_regex_partial_match(re_sct_status, sv, &name)) {
				p.set_name("ata_sct_capabilities/value/_present", name, name);
				p.value = true;  // bool

			} else if (app_regex_partial_match(re_sct_control, sv, &name)) {
				p.set_name("ata_sct_capabilities/feature_control_supported", name, name);
				p.value = true;  // bool

			} else if (app_regex_partial_match(re_sct_data, sv, &name)) {
				p.set_name("ata_sct_capabilities/data_table_supported", name, name);
				p.value = true;  // bool
			}

			if (!p.empty())
				add_property(p);
		}

		return {};
	}

	debug_out_error("app", DBG_FUNC_MSG << "Capability-section property has invalid value type.\n");

	return hz::Unexpected(SmartctlParserError::DataError, "Capability-section property has invalid value type.");
}





// -------------------- Attributes

hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_attributes(const std::string& sub)
{
	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::AtaAttributes;

	// split to lines
	std::vector<std::string> lines;
	hz::string_split(sub, '\n', lines, true);

	// Format notes:
	// * Before 5.1-14, no UPDATED column was present in "old" format.

	// * Most, but not all attribute names are with underscores. However, I encountered one
	// named "Head flying hours" and there are slashes sometimes as well.
	// So, parse until we encounter the next column. Supported in Old format only.

	// * SSD drives may show "---" in value/worst/threshold fields.

	// "old" format (used in -a):
/*
SMART Attributes Data Structure revision number: 4
Vendor Specific SMART Attributes with Thresholds:
ID# ATTRIBUTE_NAME          FLAG     VALUE WORST THRESH TYPE      UPDATED  WHEN_FAILED RAW_VALUE
  5 Reallocated_Sector_Ct   0x0032   100   100   ---    Old_age   Always       -       0
  9 Power_On_Hours          0x0032   253   100   ---    Old_age   Always       -       1720
*/

	// "brief" format (used in -x):
/*
SMART Attributes Data Structure revision number: 16
Vendor Specific SMART Attributes with Thresholds:
ID# ATTRIBUTE_NAME          FLAGS    VALUE WORST THRESH FAIL RAW_VALUE
  1 Raw_Read_Error_Rate     PO-R--   100   100   062    -    0
  2 Throughput_Performance  P-S---   197   197   040    -    160
194 Temperature_Celsius     -O----   222   222   000    -    27 (Min/Max 12/48)
                            ||||||_ K auto-keep
                            |||||__ C event count
                            ||||___ R error rate
                            |||____ S speed/performance
                            ||_____ O updated online
                            |______ P prefailure warning
*/
	enum {
		FormatStyleOld,
		FormatStyleNoUpdated,  // old format without UPDATED column
		FormatStyleBrief
	};

	bool attr_found = false;  // at least one attribute was found
	int attr_format_style = FormatStyleOld;

	const std::string space_re = "[ \\t]+";

	const std::string old_flag_re = "(0x[a-fA-F0-9]+)";
	const std::string brief_flag_re = "([A-Z+-]{2,})";
	// We allow name with spaces only in the old format, not in brief.
	// This has to do with the name end detection - it's either 0x (flag's start) in the old format,
	// or a space in the brief format.
	const std::string old_base_re = R"([ \t]*([0-9]+) ([^ \t\n]+(?:[^0-9\t\n]+)*))" + space_re + old_flag_re + space_re;  // ID / name / flag
	const std::string brief_base_re = R"([ \t]*([0-9]+) ([^ \t\n]+))" + space_re + brief_flag_re + space_re;  // ID / name / flag
	const std::string vals_re = "([0-9-]+)" + space_re + "([0-9-]+)" + space_re + "([0-9-]+)" + space_re;  // value / worst / threshold
	const std::string type_re = "([^ \\t\\n]+)" + space_re;
	const std::string updated_re = "([^ \\t\\n]+)" + space_re;
	const std::string failed_re = "([^ \\t\\n]+)" + space_re;
	const std::string raw_re = "(.+)[ \\t]*";

	const auto re_old_up = app_regex_re("/" + old_base_re + vals_re + type_re + updated_re + failed_re + raw_re + "/mi");
	const auto re_old_noup = app_regex_re("/" + old_base_re + vals_re + type_re + failed_re + raw_re + "/mi");
	const auto re_brief = app_regex_re("/" + brief_base_re + vals_re + failed_re + raw_re + "/mi");

	const auto re_flag_descr = app_regex_re("/^[\\t ]+\\|/mi");


	for (const auto& line : lines) {
		// skip the non-informative lines
		if (line.empty() || app_regex_partial_match("/SMART Attributes with Thresholds/mi", line))
			continue;

		if (app_regex_partial_match("/ATTRIBUTE_NAME/mi", line)) {
			// detect format type
			if (!app_regex_partial_match("/WHEN_FAILED/mi", line)) {
				attr_format_style = FormatStyleBrief;
			} else if (!app_regex_partial_match("/UPDATED/mi", line)) {
				attr_format_style = FormatStyleNoUpdated;
			}
			continue;  // we don't need this line
		}

		if (app_regex_partial_match(re_flag_descr, line)) {
			continue;  // skip flag description lines
		}

		if (app_regex_partial_match("/Data Structure revision number/mi", line)) {
			const auto re = app_regex_re("/^([^:\\n]+):[ \\t]*(.*)$/mi");
			std::string name, value;
			if (app_regex_partial_match(re, line, {&name, &value})) {
				hz::string_trim(name);
				hz::string_trim(value);
				int64_t value_num = 0;
				hz::string_is_numeric_nolocale(value, value_num, false);

				StorageProperty p(pt);
				p.set_name("ata_smart_attributes/revision", name, name);
				p.reported_value = value;
				p.value = value_num;  // integer-type value

				add_property(p);
				attr_found = true;
			}


		} else {  // A line in attribute table

			std::string id, name, flag, value, worst, threshold, attr_type,
					update_type, when_failed, raw_value;

			bool matched = true;

			if (attr_format_style == FormatStyleOld) {
				if (!app_regex_full_match(re_old_up, line,
						{&id, &name, &flag, &value, &worst, &threshold, &attr_type, &update_type, &when_failed, &raw_value})) {
					matched = false;
					debug_out_warn("app", DBG_FUNC_MSG << "Cannot parse attribute line.\n");
				}

			} else if (attr_format_style == FormatStyleNoUpdated) {
				if (!app_regex_full_match(re_old_noup, line,
						{&id, &name, &flag, &value, &worst, &threshold, &attr_type, &when_failed, &raw_value})) {
					matched = false;
					debug_out_warn("app", DBG_FUNC_MSG << "Cannot parse attribute line.\n");
				}

			} else if (attr_format_style == FormatStyleBrief) {
				if (!app_regex_full_match(re_brief, line,
						{&id, &name, &flag, &value, &worst, &threshold, &when_failed, &raw_value})) {
					matched = false;
					debug_out_warn("app", DBG_FUNC_MSG << "Cannot parse attribute line.\n");
				}
			}

			if (!matched) {
				debug_out_dump("app", "------------ Begin unparsable attribute line dump ------------\n");
				debug_out_dump("app", line << "\n");
				debug_out_dump("app", "------------- End unparsable attribute line dump -------------\n");
				continue;  // continue to the next line
			}


			AtaStorageAttribute attr;
			hz::string_is_numeric_nolocale(hz::string_trim_copy(id), attr.id, true, 10);
			attr.flag = hz::string_trim_copy(flag);
			uint8_t norm_value = 0, worst_value = 0, threshold_value = 0;

			if (hz::string_is_numeric_nolocale(hz::string_trim_copy(value), norm_value, true, 10)) {
				attr.value = norm_value;
			}
			if (hz::string_is_numeric_nolocale(hz::string_trim_copy(worst), worst_value, true, 10)) {
				attr.worst = worst_value;
			}
			if (hz::string_is_numeric_nolocale(hz::string_trim_copy(threshold), threshold_value, true, 10)) {
				attr.threshold = threshold_value;
			}

			if (attr_format_style == FormatStyleBrief) {
				attr.attr_type = app_regex_partial_match("/P/", attr.flag) ? AtaStorageAttribute::AttributeType::Prefail : AtaStorageAttribute::AttributeType::OldAge;
			} else {
				if (attr_type == "Pre-fail") {
					attr.attr_type = AtaStorageAttribute::AttributeType::Prefail;
				} else if (attr_type == "Old_age") {
					attr.attr_type = AtaStorageAttribute::AttributeType::OldAge;
				} else {
					attr.attr_type = AtaStorageAttribute::AttributeType::Unknown;
				}
			}

			if (attr_format_style == FormatStyleBrief) {
				attr.update_type = app_regex_partial_match("/O/", attr.flag) ? AtaStorageAttribute::UpdateType::Always : AtaStorageAttribute::UpdateType::Offline;
			} else {
				if (update_type == "Always") {
					attr.update_type = AtaStorageAttribute::UpdateType::Always;
				} else if (update_type == "Offline") {
					attr.update_type = AtaStorageAttribute::UpdateType::Offline;
				} else {
					attr.update_type = AtaStorageAttribute::UpdateType::Unknown;
				}
			}

			attr.when_failed = AtaStorageAttribute::FailTime::Unknown;
			hz::string_trim(when_failed);
			if (when_failed == "-") {
				attr.when_failed = AtaStorageAttribute::FailTime::None;
			} else if (when_failed == "In_the_past" || when_failed == "Past") {  // the second one if from brief format
				attr.when_failed = AtaStorageAttribute::FailTime::Past;
			} else if (when_failed == "FAILING_NOW" || when_failed == "NOW") {  // the second one if from brief format
				attr.when_failed = AtaStorageAttribute::FailTime::Now;
			}

			attr.raw_value = hz::string_trim_copy(raw_value);
			hz::string_is_numeric_nolocale(hz::string_trim_copy(raw_value), attr.raw_value_int, false);  // same as raw_value, but parsed as int.

			StorageProperty p(pt);
			p.set_name(hz::string_trim_copy(name), hz::string_trim_copy(name), hz::string_trim_copy(name));
			p.reported_value = line;  // use the whole line here
			p.value = attr;  // attribute-type value;

			add_property(p);
			attr_found = true;
		}

	}

	if (!attr_found) {
		return hz::Unexpected(SmartctlParserError::DataError, "No attributes found in Attributes section.");
	}

	return {};
}



hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_directory_log(const std::string& sub)
{
	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::DirectoryLog;

	// Directory log contains:
/*
General Purpose Log Directory Version 1
SMART           Log Directory Version 1 [multi-sector log support]
Address    Access  R/W   Size  Description
0x00       GPL,SL  R/O      1  Log Directory
0x01           SL  R/O      1  Summary SMART error log
0x02           SL  R/O      5  Comprehensive SMART error log
0x03       GPL     R/O      6  Ext. Comprehensive SMART error log
0x04       GPL,SL  R/O      8  Device Statistics log
0x06           SL  R/O      1  SMART self-test log
0x07       GPL     R/O      1  Extended self-test log
0x09           SL  R/W      1  Selective self-test log
0x0a       GPL     R/W      8  Device Statistics Notification
*/
//	bool data_found = false;  // true if something was found.

	// the whole subsection
	{
		StorageProperty p(pt);
		p.set_name("ata_log_directory/_merged", "General Purpose Log Directory");
		p.reported_value = sub;
		p.value = p.reported_value;  // string-type value

		add_property(p);
//		data_found = true;
	}

	// supported / unsupported
	{
		StorageProperty p(pt);
		p.set_name("_text_only/directory_log_supported", "General Purpose Log Directory supported");

		// p.reported_value;  // nothing
		p.value = !app_regex_partial_match("/General Purpose Log Directory not supported/mi", sub);  // bool

		add_property(p);
//		data_found = true;
	}

//	return data_found;
	return {};
}



hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_error_log(const std::string& sub)
{
	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::AtaErrorLog;

	// Note: The format of this section was changed somewhere between 5.0-x and 5.30.
	// The old format is doesn't really give any useful info, and whatever's left is somewhat
	// parsable by this parser. Can't really improve that.
	// Also, type (e.g. UNC) is not always present (depends on the drive I guess).

	// Sample "-l xerror" output:
/*
SMART Extended Comprehensive Error Log Version: 1 (1 sectors)
Device Error Count: 1
	CR     = Command Register
	FEATR  = Features Register
	COUNT  = Count (was: Sector Count) Register
	LBA_48 = Upper bytes of LBA High/Mid/Low Registers ]  ATA-8
	LH     = LBA High (was: Cylinder High) Register    ]   LBA
	LM     = LBA Mid (was: Cylinder Low) Register      ] Register
	LL     = LBA Low (was: Sector Number) Register     ]
	DV     = Device (was: Device/Head) Register
	DC     = Device Control Register
	ER     = Error register
	ST     = Status register
Powered_Up_Time is measured from power on, and printed as
DDd+hh:mm:SS.sss where DD=days, hh=hours, mm=minutes,
SS=sec, and sss=millisec. It "wraps" after 49.710 days.

Error 1 [0] occurred at disk power-on lifetime: 1 hours (0 days + 1 hours)
  When the command that caused the error occurred, the device was active or idle.

  After command completion occurred, registers were:
  ER -- ST COUNT  LBA_48  LH LM LL DV DC
  -- -- -- == -- == == == -- -- -- -- --
  02 -- 51 00 00 00 00 00 00 00 00 00 00  Error: TK0NF

  Commands leading to the command that caused the error were:
  CR FEATR COUNT  LBA_48  LH LM LL DV DC  Powered_Up_Time  Command/Feature_Name
  -- == -- == -- == == == -- -- -- -- --  ---------------  --------------------
  10 00 00 00 01 00 00 00 00 03 34 e0 ff     00:00:17.305  RECALIBRATE [OBS-4]
  10 00 00 00 01 00 00 00 00 03 34 e0 08     00:00:17.138  RECALIBRATE [OBS-4]
  91 40 00 01 3f 00 00 01 00 03 34 af 08     00:00:17.138  INITIALIZE DEVICE PARAMETERS [OBS-6]
  c4 00 40 00 00 00 00 3f 00 00 00 e0 04     00:00:16.934  READ MULTIPLE
  c4 00 40 00 01 00 00 3f 00 00 00 e0 00     00:00:07.959  READ MULTIPLE
*/
	bool data_found = false;

	// Error log version
	{
		// "SMART Error Log Version: 1"
		// "SMART Extended Comprehensive Error Log Version: 1 (1 sectors)"
		const auto re = app_regex_re("/^(SMART (Extended Comprehensive )?Error Log Version): ([0-9]+).*?$/mi");

		std::string name, value;
		if (app_regex_partial_match(re, sub, {&name, &value})) {
			hz::string_trim(name);
			hz::string_trim(value);

			StorageProperty p(pt);
			// Note: For extended logs, the path has "extended".
			// For standard logs, the path has "summary" (?)
			p.set_name("ata_smart_error_log/extended/revision", name, name);
			p.reported_value = value;

			int64_t value_num = 0;
			hz::string_is_numeric_nolocale(value, value_num, false);
			p.value = value_num;  // integer

			add_property(p);
			data_found = true;
		}
	}

	// Error log support
	{
		const auto re = app_regex_re("/^(Warning: device does not support Error Logging)|(SMART Error Log not supported)$/mi");

		if (app_regex_partial_match(re, sub)) {
			StorageProperty p(pt);
			p.set_name("_text_only/ata_smart_error_log/_not_present", "Error Log not supported");
			p.displayable_name = "Warning";
			p.readable_value = "Device does not support error logging";
			add_property(p);
		}
	}

	// Error log entry count
	{
		// note: these represent the same information
		const auto re1 = app_regex_re("/^(?:ATA|Device) Error Count:[ \\t]*([0-9]+)/mi");
		const auto re2 = app_regex_re("/^No Errors Logged$/mi");

		std::string value;
		if (app_regex_partial_match(re1, sub, &value) || app_regex_partial_match(re2, sub)) {
			hz::string_trim(value);

			StorageProperty p(pt);
			// Note: For Extended Error Log, the path has "extended".
			// For simple error log, the path has "summary".
			p.set_name("ata_smart_error_log/extended/count", "ATA Error Count");
			p.reported_value = value;

			int64_t value_num = 0;
			if (!app_regex_partial_match(re2, sub)) {  // if no errors, when value should be zero. otherwise, this:
				hz::string_is_numeric_nolocale(value, value_num, false);
			}
			p.value = value_num;  // integer

			add_property(p);
			data_found = true;
		}
	}

	// Individual errors
	{
		// Split by blocks:
		// "Error 1 [0] occurred at disk power-on lifetime: 1 hours (0 days + 1 hours)"
		// "Error 25 occurred at disk power-on lifetime: 14799 hours"
		const auto re_block = app_regex_re(
				R"(/^((Error[ \t]*([0-9]+))[ \t]*(?:\[[0-9]+\][ \t])?occurred at disk power-on lifetime:[ \t]*([0-9]+) hours(?:[^\n]*)?.*(?:\n(?:  |\n  ).*)*)/mi)");

		// "  When the command that caused the error occurred, the device was active or idle."
		// Note: For "in an unknown state" - remove first two words.
		const auto re_state = app_regex_re(R"(/occurred, the device was[ \t]*(?: in)?(?: an?)?[ \t]+([^.\n]*)\.?/mi)");
		// "  84 51 2c 71 cd 3f e6  Error: ICRC, ABRT 44 sectors at LBA = 0x063fcd71 = 104844657"
		// "  40 51 00 f5 41 61 e0  Error: UNC at LBA = 0x006141f5 = 6373877"
		// "  02 -- 51 00 00 00 00 00 00 00 00 00 00  Error: TK0NF"
		const auto re_type = app_regex_re(R"(/[ \t]+Error:[ \t]*([ ,a-z0-9]+)(?:[ \t]+((?:[0-9]+|at )[ \t]*.*))?$/mi)");

		for (auto it = std::sregex_iterator(sub.begin(), sub.end(), re_block), end = std::sregex_iterator(); it != end; ++it) {
			const std::string block = hz::string_trim_copy(it->str(1));
			const std::string name = hz::string_trim_copy(it->str(2));
			const std::string value_num = hz::string_trim_copy(it->str(3));
			const std::string value_time = hz::string_trim_copy(it->str(4));

			// debug_out_dump("app", "\nBLOCK -------------------------------\n" << block);

			std::string state, etypes_str, emore;
			app_regex_partial_match(re_state, block, &state);
			app_regex_partial_match(re_type, block, {&etypes_str, &emore});

			StorageProperty p(pt);
			std::string gen_name = hz::string_trim_copy(name);
			p.set_name(gen_name, gen_name, gen_name);  // "Error 6"
			p.reported_value = block;

			AtaStorageErrorBlock eb;
			hz::string_is_numeric_nolocale(value_num, eb.error_num, false);
			hz::string_is_numeric_nolocale(value_time, eb.lifetime_hours, false);

			std::vector<std::string> etypes;
			hz::string_split(etypes_str, ",", etypes, true);
			for (auto&& v : etypes) {
				hz::string_trim(v);
			}

			eb.device_state = hz::string_trim_copy(state);
			eb.reported_types = etypes;
			eb.type_more_info = hz::string_trim_copy(emore);

			p.value = eb;  // Error block value

			add_property(p);
			data_found = true;
		}


	}

	// the whole subsection
	{
		StorageProperty p(pt);
		p.set_name("ata_smart_error_log/_merged", "SMART Error Log");
		p.reported_value = sub;
		p.value = p.reported_value;  // string-type value

		add_property(p);
		// data_found = true;
	}

	// We may further split this subsection by Error blocks, but it's unnecessary -
	// the data is too advanced to be of any use if parsed.

	if (!data_found) {
		return hz::Unexpected(SmartctlParserError::DataError, "No error log entries found in Error Log section.");
	}

	return {};
}




// -------------------- Selftest Log

hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_selftest_log(const std::string& sub)
{
	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::SelftestLog;

	// Self-test log contains:
	// * structure revision number
	// * a list of current / previous tests performed, with each having:
	// num (the higher - the older).
	// test_description (Extended offline / Short offline / Conveyance offline / ... ?)
	// status (completed without error, interrupted (reason), aborted, fatal or unknown error, ?)
	// remaining % (this will be 00% for completed, and may be > 0 for interrupted).
	// lifetime (hours) - int.
	// LBA_of_first_error - "-" or int ?
/*
SMART Extended Self-test Log Version: 1 (1 sectors)
Num  Test_Description    Status                  Remaining  LifeTime(hours)  LBA_of_first_error
# 1  Extended offline    Completed without error       00%     43116         -
# 2  Extended offline    Completed without error       00%     29867         -
# 3  Extended offline    Completed without error       00%     19477         -
*/

	bool data_found = false;  // true if something was found.

	// The whole subsection
	{
		StorageProperty p(pt);
		p.set_name("ata_smart_self_test_log/_merged", "SMART Self-Test Log");
		p.reported_value = sub;
		p.value = p.reported_value;  // string-type value

		add_property(p);
//		data_found = true;
	}


	// Self-test log support
	{
		const auto re = app_regex_re("/^(Warning: device does not support Self Test Logging)|(SMART Self-test Log not supported)$/mi");

		if (app_regex_partial_match(re, sub)) {
			StorageProperty p(pt);
			p.set_name("ata_smart_self_test_log/_present", "Self-test Log supported");
			p.displayable_name = "Warning";
			p.readable_value = "Device does not support self-test logging";
			add_property(p);

			data_found = true;
		}
	}

	// Self-test log version
	{
		// SMART Self-test log structure revision number 1
		// SMART Extended Self-test Log Version: 1 (1 sectors)
		const auto re1 = app_regex_re(R"(/(SMART Self-test log structure[^\n0-9]*)([^ \n]+)[ \t]*$/mi)");
		const auto re1_ex = app_regex_re("/(SMART Extended Self-test Log Version): ([0-9]+).*$/mi");
		// older smartctl (pre 5.1-16)
		const auto re2 = app_regex_re(R"(/(SMART Self-test log, version number[^\n0-9]*)([^ \n]+)[ \t]*$/mi)");

		std::string name, value;
		if (app_regex_partial_match(re1, sub, {&name, &value})
				|| app_regex_partial_match(re1_ex, sub, {&name, &value})
				|| app_regex_partial_match(re2, sub, {&name, &value})) {
			hz::string_trim(value);

			StorageProperty p(pt);
			p.set_name("ata_smart_self_test_log/extended/revision", hz::string_trim_copy(name));
			p.reported_value = value;

			int64_t value_num = 0;
			hz::string_is_numeric_nolocale(value, value_num, false);
			p.value = value_num;  // integer

			add_property(p);
			data_found = true;
		}
	}


	int64_t test_count = 0;  // type is of p.value_integer


	// individual entries
	{
		// split by columns.
		// num, type, status, remaining, hours, lba (optional).
		const auto re = app_regex_re(
				R"(/^(#[ \t]*([0-9]+)[ \t]+(\S+(?: \S+)*)  [ \t]*(\S.*) [ \t]*([0-9]+%)  [ \t]*([0-9]+)[ \t]*((?:  [ \t]*\S.*)?))$/mi)");

		for (auto it = std::sregex_iterator(sub.begin(), sub.end(), re), end = std::sregex_iterator(); it != end; ++it) {
			const std::string line = hz::string_trim_copy(it->str(1));
			const std::string num = hz::string_trim_copy(it->str(2));
			const std::string type = hz::string_trim_copy(it->str(3));
			const std::string status_str = hz::string_trim_copy(it->str(4));
			const std::string remaining = hz::string_trim_copy(it->str(5));
			const std::string hours = hz::string_trim_copy(it->str(6));
			const std::string lba = hz::string_trim_copy(it->str(7));

			StorageProperty p(pt);
			p.set_name(fmt::format("ata_smart_self_test_log/entry/{}", num), "Self-test entry " + num);
			p.reported_value = hz::string_trim_copy(line);

			AtaStorageSelftestEntry sse;

			hz::string_is_numeric_nolocale(num, sse.test_num, false);
			hz::string_is_numeric_nolocale(hz::string_trim_copy(remaining), sse.remaining_percent, false);
			hz::string_is_numeric_nolocale(hz::string_trim_copy(hours), sse.lifetime_hours, false);

			sse.type = hz::string_trim_copy(type);
			sse.lba_of_first_error = hz::string_trim_copy(lba);
			// old smartctls didn't print anything for lba if none, but newer ones print "-". normalize.
			if (sse.lba_of_first_error.empty())
				sse.lba_of_first_error = "-";

			AtaStorageSelftestEntry::Status status = AtaStorageSelftestEntry::Status::Unknown;

			// don't match end - some of them are not complete here
			if (app_regex_partial_match("/^Completed without error/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::CompletedNoError;
			} else if (app_regex_partial_match("/^Aborted by host/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::AbortedByHost;
			} else if (app_regex_partial_match("/^Interrupted \\(host reset\\)/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::Interrupted;
			} else if (app_regex_partial_match("/^Fatal or unknown error/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::FatalOrUnknown;
			} else if (app_regex_partial_match("/^Completed: unknown failure/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::ComplUnknownFailure;
			} else if (app_regex_partial_match("/^Completed: electrical failure/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::ComplElectricalFailure;
			} else if (app_regex_partial_match("/^Completed: servo\\/seek failure/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::ComplServoFailure;
			} else if (app_regex_partial_match("/^Completed: read failure/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::ComplReadFailure;
			} else if (app_regex_partial_match("/^Completed: handling damage/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::ComplHandlingDamage;
			} else if (app_regex_partial_match("/^Self-test routine in progress/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::InProgress;
			} else if (app_regex_partial_match("/^Unknown\\/reserved test status/mi", status_str)) {
				status = AtaStorageSelftestEntry::Status::Reserved;
			}

			sse.status_str = status_str;
			sse.status = status;

			p.value = sse;  // AtaStorageSelftestEntry value

			add_property(p);
			data_found = true;

			++test_count;
		}
	}


	// number of tests.
	// Note: "No self-tests have been logged" is sometimes absent, so don't rely on it.
	{
		StorageProperty p(pt);
		p.set_name("ata_smart_self_test_log/extended/table/count", "Number of entries in self-test log");
		// p.reported_value;  // nothing
		p.value = test_count;  // integer

		add_property(p);

		if (test_count > 0) {
			data_found = true;
		}
	}

	if (!data_found) {
		return hz::Unexpected(SmartctlParserError::DataError, "No self-test log entries found in Self-test Log section.");
	}

	return {};
}




// -------------------- Selective Selftest Log

hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_selective_selftest_log(const std::string& sub)
{
	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::SelectiveSelftestLog;

	// Selective self-test log contains:
/*
SMART Selective self-test log data structure revision number 1
 SPAN  MIN_LBA  MAX_LBA  CURRENT_TEST_STATUS
    1        0        0  Not_testing
    2        0        0  Not_testing
    3        0        0  Not_testing
    4        0        0  Not_testing
    5        0        0  Not_testing
Selective self-test flags (0x0):
  After scanning selected spans, do NOT read-scan remainder of disk.
If Selective self-test is pending on power-up, resume after 0 minute delay.
*/

	bool data_found = false;  // true if something was found.

	// the whole subsection
	{
		StorageProperty p(pt);
		p.set_name("ata_smart_selective_self_test_log/_merged", "SMART selective self-test log");
		p.reported_value = sub;
		p.value = p.reported_value;  // string-type value

		add_property(p);
		// data_found = true;
	}

	// supported / unsupported
	{
		StorageProperty p(pt);
		p.set_name("ata_smart_data/capabilities/selective_self_test_supported", "Selective self-tests supported");

		// p.reported_value;  // nothing
		p.value = !app_regex_partial_match("/Device does not support Selective Self Tests\\/Logging/mi", sub);  // bool

		add_property(p);

		if (!p.get_value<bool>()) {
			data_found = true;
		}
	}

	if (!data_found) {
		return hz::Unexpected(SmartctlParserError::DataError, "No selective self-test log entries found in Selective Self-test Log section.");
	}

	return {};
}



hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_scttemp_log(const std::string& sub)
{
	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::TemperatureLog;

	// scttemp log contains:
/*
SCT Status Version:                  3
SCT Version (vendor specific):       258 (0x0102)
SCT Support Level:                   1
Device State:                        Active (0)
Current Temperature:                    39 Celsius
Power Cycle Min/Max Temperature:     25/39 Celsius
Lifetime    Min/Max Temperature:     17/50 Celsius
Under/Over Temperature Limit Count:   0/0
Vendor specific:
01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

SCT Temperature History Version:     2
Temperature Sampling Period:         1 minute
Temperature Logging Interval:        1 minute
Min/Max recommended Temperature:      0/60 Celsius
Min/Max Temperature Limit:           -41/85 Celsius
Temperature History Size (Index):    478 (361)

Index    Estimated Time   Temperature Celsius
 362    2017-08-29 08:43    38  *******************
 ...    ..(119 skipped).    ..  *******************
   4    2017-08-29 10:43    38  *******************
   5    2017-08-29 10:44    39  ********************
 ...    ..( 91 skipped).    ..  ********************
  97    2017-08-29 12:16    39  ********************
  98    2017-08-29 12:17     ?  -
  99    2017-08-29 12:18    25  ******
*/
	bool data_found = false;  // true if something was found.

	// the whole subsection
	{
		StorageProperty p(pt);
		p.set_name("ata_sct_status/_and/ata_sct_temperature_history/_merged", "SCT temperature log");
		p.reported_value = sub;
		p.value = p.reported_value;  // string-type value

		add_property(p);
//		data_found = true;
	}

	// supported / unsupported
	{
		StorageProperty p(pt);
		p.set_name("_text_only/ata_sct_status/_not_present", "SCT commands unsupported");

		// p.reported_value;  // nothing
		p.value = app_regex_partial_match("/(SCT Commands not supported)|(SCT Data Table command not supported)/mi", sub);  // bool

		add_property(p);

		if (p.get_value<bool>()) {
			data_found = true;
		}
	}

	// Find current temperature
	{
		std::string name, value;
		if (app_regex_partial_match("/^(Current Temperature):[ \\t]+(.*) Celsius$/mi", sub, {&name, &value})) {
			StorageProperty p;
			p.section = StoragePropertySection::TemperatureLog;
			p.set_name("ata_sct_status/temperature/current", "Current Temperature");
			p.reported_value = value;
			p.value = hz::string_to_number_nolocale<int64_t>(value);  // integer
			add_property(p);

			data_found = true;
		}
	}

	if (!data_found) {
		return hz::Unexpected(SmartctlParserError::DataError, "No temperature log entries found in SCT Temperature Log section.");
	}

	return {};
}



hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_scterc_log(const std::string& sub)
{
	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::ErcLog;

	// scterc log contains:
/*
SCT Error Recovery Control:
           Read:     70 (7.0 seconds)
          Write:     70 (7.0 seconds)
*/
	bool data_found = false;  // true if something was found.

	// the whole subsection
	{
		StorageProperty p(pt);
		p.set_name("ata_sct_erc/_merged", "SCT ERC log");
		p.reported_value = sub;
		p.value = p.reported_value;  // string-type value

		add_property(p);
		// data_found = true;
	}

	// supported / unsupported
	{
		StorageProperty p(pt);
		p.set_name("ata_sct_erc/_present", "SCT ERC supported");

		// p.reported_value;  // nothing
		p.value = !app_regex_partial_match("/SCT Error Recovery Control command not supported/mi", sub);  // bool

		add_property(p);

		if (p.get_value<bool>()) {
			data_found = true;
		}
	}

	if (!data_found) {
		return hz::Unexpected(SmartctlParserError::DataError, "No entries found in SCT ERC Log section.");
	}

	return {};
}



hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_devstat(const std::string& sub)
{
	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::Statistics;

	// devstat log contains:
/*
Device Statistics (GP Log 0x04)
Page  Offset Size        Value Flags Description
0x01  =====  =               =  ===  == General Statistics (rev 1) ==
0x01  0x008  4             569  -D-  Lifetime Power-On Resets
0x01  0x010  4            6360  -D-  Power-on Hours
0x01  0x018  6     17887792526  -D-  Logical Sectors Written
0x01  0x020  6        51609191  -D-  Number of Write Commands
0x01  0x028  6     17634698564  -D-  Logical Sectors Read
0x01  0x030  6       179799274  -D-  Number of Read Commands
0x01  0x038  6      1421163520  -D-  Date and Time TimeStamp
0x01  0x048  2             202  ND-  Workload Utilization
0x03  =====  =               =  ===  == Rotating Media Statistics (rev 1) ==
0x03  0x008  4            6356  -D-  Spindle Motor Power-on Hours
0x03  0x010  4            6356  -D-  Head Flying Hours
                                |||_ C monitored condition met
                                ||__ D supports DSN
                                |___ N normalized value
*/

	// Old (6.3) format:
/*
Page Offset Size         Value  Description
  1  =====  =                =  == General Statistics (rev 2) ==
  1  0x008  4                2  Lifetime Power-On Resets
  1  0x018  6       1480289770  Logical Sectors Written
  1  0x020  6         28939977  Number of Write Commands
  1  0x028  6          3331436  Logical Sectors Read
  1  0x030  6           122181  Number of Read Commands
  1  0x038  6      12715200000  Date and Time TimeStamp
  7  =====  =                =  == Solid State Device Statistics (rev 1) ==
  7  0x008  1                1~ Percentage Used Endurance Indicator
                              |_ ~ normalized value
*/

	enum {
		FormatStyleNoFlags,  // 6.3 and older
		FormatStyleCurrent,  // 6.5
	};


	// supported / unsupported
	bool supported = true;
	{
		StorageProperty p(pt);
		p.set_name("ata_device_statistics/_present", "Device statistics supported");

		// p.reported_value;  // nothing
		supported = !app_regex_partial_match(R"(/Device Statistics \(GP\/SMART Log 0x04\) not supported/mi)", sub);
		p.value = supported;  // bool

		add_property(p);
	}

	if (!supported) {
		return hz::Unexpected(SmartctlParserError::DataError, "Device statistics not supported.");
	}

	bool entries_found = false;  // at least one entry was found

	// split to lines
	std::vector<std::string> lines;
	hz::string_split(sub, '\n', lines, true);

	const std::string space_re = "[ \\t]+";

	const std::string flag_re = "([A-Z=-]{3,})";
	// Page Offset Size Value Flags Description
	const auto line_re = app_regex_re("/[ \\t]*([0-9a-z]+)" + space_re + "([0-9a-z=]+)" + space_re + "([0-9=]+)"
			+ space_re + "([0-9=-]+)" + space_re + flag_re + space_re + "(.+)/mi");
	// Page Offset Size Value Description
	const auto line_re_noflags = app_regex_re("/[ \\t]*([0-9a-z]+)" + space_re + "([0-9a-z=]+)" + space_re + "([0-9=]+)"
			+ space_re + "([0-9=~-]+)" + space_re + "(.+)/mi");
	// flag description lines
	const auto re_flag_descr = app_regex_re("/^[\\t ]+\\|/mi");


	int devstat_format_style = FormatStyleCurrent;

	for (const auto& line : lines) {
		// skip the non-informative lines
		// "Device Statistics (GP Log 0x04)"
		// "Device Statistics (SMART Log 0x04)"
		// "ATA_SMART_READ_LOG failed: Undefined error: 0"
		// "Read Device Statistics page 0x00 failed"
		// "Read Device Statistics pages 0x00-0x07 failed"
		if (line.empty()
				|| app_regex_partial_match("/^Device Statistics \\((?:GP|SMART) Log 0x04\\)/mi", line)
				|| app_regex_partial_match("/^ATA_SMART_READ_LOG failed:/mi", line)
				|| app_regex_partial_match("/^Read Device Statistics page (?:.+) failed/mi", line)
				|| app_regex_partial_match("/^Read Device Statistics pages (?:.+) failed/mi", line) ) {
			continue;
		}

		// Table header
		if (app_regex_partial_match("/^Page[\\t ]+Offset[\\t ]+Size/mi", line)) {
			// detect format type
			if (!app_regex_partial_match("/[\\t ]+Flags[\\t ]+/mi", line)) {
				devstat_format_style = FormatStyleNoFlags;
			}
			continue;  // we don't need this line
		}

		if (app_regex_partial_match(re_flag_descr, line)) {  // "    |||_ C monitored condition met", etc.
			continue;  // skip flag description lines
		}

		std::string page, offset, size, value, flags, description;

		bool matched = false;
		if (devstat_format_style == FormatStyleCurrent) {
			if (app_regex_full_match(line_re, line, {&page, &offset, &size, &value, &flags, &description})) {
				matched = true;
			}
		} else if (devstat_format_style == FormatStyleNoFlags) {
			if (app_regex_full_match(line_re_noflags, line, {&page, &offset, &size, &value, &description})) {
				matched = true;
				flags = "---";  // to keep consistent with the Current format
				if (!value.empty() && value[value.size() - 1] == '~') {  // normalized
					flags = "N--";
					value.resize(value.size() - 1);
				}
			}
		}

		if (!matched) {
			debug_out_warn("app", DBG_FUNC_MSG << "Cannot parse devstat line.\n");
			debug_out_dump("app", "------------ Begin unparsable devstat line dump ------------\n");
			debug_out_dump("app", line << "\n");
			debug_out_dump("app", "------------- End unparsable devstat line dump -------------\n");
			continue;  // continue to the next line
		}


		AtaStorageStatistic st;
		st.is_header = (hz::string_trim_copy(value) == "=");
		st.flags = st.is_header ? std::string() : hz::string_trim_copy(flags);
		st.value = st.is_header ? std::string() : hz::string_trim_copy(value);
		hz::string_is_numeric_nolocale(st.value, st.value_int, false);
		hz::string_is_numeric_nolocale(page, st.page, false, 16);
		hz::string_is_numeric_nolocale(offset, st.offset, false, 16);

		if (st.is_header) {
			description = hz::string_trim_copy(hz::string_trim_copy(description, "="));
		}

		StorageProperty p(pt);
		std::string gen_name = hz::string_trim_copy(description);
		p.set_name(gen_name, gen_name, gen_name);
		p.reported_value = line;  // use the whole line here
		p.value = st;  // statistic-type value

		add_property(p);
		entries_found = true;
	}

	if (!entries_found) {
		return hz::Unexpected(SmartctlParserError::DataError, "No entries found in Device Statistics section.");
	}

	return {};
}



hz::ExpectedVoid<SmartctlParserError> SmartctlTextAtaParser::parse_section_data_subsection_sataphy(const std::string& sub)
{
	StorageProperty pt;  // template for easy copying
	pt.section = StoragePropertySection::PhyLog;

	// sataphy log contains:
/*
SATA Phy Event Counters (GP Log 0x11)
ID      Size     Value  Description
0x0001  2            0  Command failed due to ICRC error
0x0002  2            0  R_ERR response for data FIS
0x0003  2            0  R_ERR response for device-to-host data FIS
0x0004  2            0  R_ERR response for host-to-device data FIS
0x0005  2            0  R_ERR response for non-data FIS
0x0006  2            0  R_ERR response for device-to-host non-data FIS
0x0007  2            0  R_ERR response for host-to-device non-data FIS
0x0008  2            0  Device-to-host non-data FIS retries
0x0009  2            1  Transition from drive PhyRdy to drive PhyNRdy
*/
	bool data_found = false;  // true if something was found.

	// the whole subsection
	{
		StorageProperty p(pt);
		p.set_name("sata_phy_event_counters/_merged", "SATA Phy log");
		p.reported_value = sub;
		p.value = p.reported_value;  // string-type value

		add_property(p);
		// data_found = true;
	}

	// supported / unsupported
	{
		StorageProperty p(pt);
		p.set_name("sata_phy_event_counters/_present", "SATA Phy log supported");

		// p.reported_value;  // nothing
		p.value = !app_regex_partial_match("/SATA Phy Event Counters \\(GP Log 0x11\\) not supported/mi", sub)
				&& !app_regex_partial_match("/SATA Phy Event Counters with [0-9-]+ sectors not supported/mi", sub);  // bool

		add_property(p);

		if (p.get_value<bool>()) {
			data_found = true;
		}
	}

	if (!data_found) {
		return hz::Unexpected(SmartctlParserError::DataError, "No entries found in SATA Phy Event Counters section.");
	}

	return {};
}



void SmartctlTextAtaParser::set_data_section_info(std::string s)
{
	data_section_info_ = std::move(s);
}



void SmartctlTextAtaParser::set_data_section_data(std::string s)
{
	data_section_data_ = std::move(s);
}






/// @}
