#pragma once

#include <optional>

#include "core/plist.h"
#include "frameworks/Foundation/foundation.h"

namespace orchard::foundation
{
Id plist_to_object(Cpu& c, const Plist& p, bool mutable_containers = false);
std::optional<Plist> object_to_plist(Cpu& c, Id obj);

}
