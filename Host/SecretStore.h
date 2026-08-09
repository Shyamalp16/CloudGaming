#pragma once

#include <optional>
#include <string>

namespace SecretStore {
bool Set(const std::string& name, const std::string& value, std::string& error);
std::optional<std::string> Get(const std::string& name, std::string& error);
bool Remove(const std::string& name, std::string& error);
}
