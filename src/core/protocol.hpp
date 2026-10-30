#pragma once

#include <string>

#include "handle.hpp"

std::string advertise(Handle& h, const std::string& service);
std::string upload_pack(Handle& h, const std::string& request);
std::string receive_pack(Handle& h, const std::string& request);
void repair_head(Handle& h, const std::string& preferred);
void touch(Handle& h);
