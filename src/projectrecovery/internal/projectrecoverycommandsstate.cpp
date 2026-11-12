/*
 * Audacity: A Digital Audio Editor
 */
#include "projectrecoverycommandsstate.h"

using namespace au::projectrecovery;
using namespace muse;
using namespace muse::rcommand;

std::string ProjectRecoveryCommandsState::moduleName() const
{
    return "projectrecovery";
}

void ProjectRecoveryCommandsState::init()
{
}

void ProjectRecoveryCommandsState::deinit()
{
}

CommandState ProjectRecoveryCommandsState::commandState(const Command&) const
{
    return CommandState(true, false);
}

async::Channel<Command, CommandState> ProjectRecoveryCommandsState::commandStateChanged() const
{
    return {};
}
