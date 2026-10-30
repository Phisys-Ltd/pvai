// pvai run and pvai attach.
#pragma once

#include <string>
#include <vector>

namespace pvai {

struct WrapperOptions {
    std::string label;
    std::string id;        // given by the daemon for the sessions it starts
    bool detached = false; // no terminal; seen through `pvai attach`
    std::vector<std::string> command;
};

int runWrapper(const WrapperOptions &options);
int runAttach(const std::string &id);
// Connects to this host's daemon, starting it first when `start` is set and
// nobody listens; -1 when there is none.
int openDaemon(bool start);

} // namespace pvai
