/******************************************************************************
License: GNU General Public License v3.0 only
Copyright:
	(C) 2022 Alexander Shaduri <ashaduri@gmail.com>
******************************************************************************/
/// \file
/// \author Alexander Shaduri
/// \ingroup applib
/// \weakgroup applib
/// @{

#ifndef SMARTCTL_PARSER_TYPES_H
#define SMARTCTL_PARSER_TYPES_H

#include "local_glibmm.h"

#include "hz/enum_helper.h"



enum class SmartctlParserType {
	JsonBasic,  ///< Info only
	JsonAta,
	TextBasic,  ///< Info only
	TextAta,
};



/// Helper structure for enum-related functions
struct SmartctlParserTypeExt
		: public hz::EnumHelper<
				SmartctlParserType,
				SmartctlParserTypeExt,
		        Glib::ustring>
{
	static constexpr inline SmartctlParserType default_value = SmartctlParserType::JsonAta;

	static std::unordered_map<EnumType, std::pair<std::string, Glib::ustring>> build_enum_map()
	{
		return {
				{SmartctlParserType::JsonBasic, {"json_basic", _("JSON Basic")}},
				{SmartctlParserType::JsonAta, {"json_ata", _("JSON ATA")}},
				{SmartctlParserType::TextBasic, {"text_basic", _("Text Basic")}},
				{SmartctlParserType::TextAta, {"text_ata", _("Text ATA")}},
		};
	}

};



enum class SmartctlParserFormat {
	Json,
	Text,
};



enum class SmartctlParserSettingType {
	Auto,
	Json,
	Text,
};



/// Helper structure for enum-related functions
struct SmartctlParserSettingTypeExt
		: public hz::EnumHelper<
				SmartctlParserSettingType,
				SmartctlParserSettingTypeExt,
		        Glib::ustring>
{
	static constexpr inline SmartctlParserSettingType default_value = SmartctlParserSettingType::Auto;

	static std::unordered_map<EnumType, std::pair<std::string, Glib::ustring>> build_enum_map()
	{
		return {
			{SmartctlParserSettingType::Auto, {"auto", _("Automatic")}},
			{SmartctlParserSettingType::Json, {"json", _("JSON")}},
			{SmartctlParserSettingType::Text, {"text", _("Text")}},
		};
	}

};




#endif

/// @}
