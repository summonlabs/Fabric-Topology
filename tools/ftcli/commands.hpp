// Fabric Topology inspection tool.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#ifndef FABRIC_TOPOLOGY_TOOLS_FTCLI_COMMANDS_HPP
#define FABRIC_TOPOLOGY_TOOLS_FTCLI_COMMANDS_HPP

#include <iosfwd>
#include <string>
#include <vector>

namespace ftcli {

/// Run one inspection command. Output is written to stdout and is fully deterministic.
int run(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& error);

}  // namespace ftcli

#endif  // FABRIC_TOPOLOGY_TOOLS_FTCLI_COMMANDS_HPP
