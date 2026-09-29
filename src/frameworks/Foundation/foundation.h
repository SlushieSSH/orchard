#pragma once

#include <string>
#include <string_view>

#include "core/memory.h"

namespace orchard
{
class Cpu;
struct Runtime;
namespace objc
{
class ObjcRuntime;
}
}

namespace orchard::foundation
{
using Id = GuestAddr;

std::u16string utf8_to_16(std::string_view s);
std::string utf16_to_8(std::u16string_view s);

std::u16string to_utf16(Cpu& cpu, Id str);
std::string to_utf8(Cpu& cpu, Id str);
bool is_string(Cpu& cpu, Id obj);

Id new_string(Cpu& cpu, std::u16string text, bool mutable_ = false);
Id new_string(Runtime& rt, std::u16string text, bool mutable_ = false);
Id string_autoreleased(Cpu& cpu, std::string_view utf8);
Id string16_autoreleased(Cpu& cpu, std::u16string text);

std::string describe(Cpu& cpu, Id obj);

void register_nsstring(objc::ObjcRuntime& rt);
void register_nsstring_extra(objc::ObjcRuntime& rt);
void register_regex(objc::ObjcRuntime& rt);
void register_map_tables(objc::ObjcRuntime& rt);
void register_collections(objc::ObjcRuntime& rt);
void register_json(objc::ObjcRuntime& rt);
void register_system(objc::ObjcRuntime& rt);
void register_runtime_classes(objc::ObjcRuntime& rt);
void register_url_session(objc::ObjcRuntime& rt);
void register_containers_io(objc::ObjcRuntime& rt);

Id date_with_reference_seconds(Cpu& c, double seconds);
bool date_seconds(Cpu& c, Id obj, double& out);
std::string url_string_path(Cpu& c, Id url);
Id new_file_url(Cpu& c, const std::string& path);

}
