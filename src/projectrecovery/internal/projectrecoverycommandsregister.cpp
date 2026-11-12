/*
 * Audacity: A Digital Audio Editor
 */
#include "projectrecoverycommandsregister.h"

#include "../projectrecoverycommands.h"

using namespace au::projectrecovery;
using namespace muse;
using namespace muse::rcommand;

std::string ProjectRecoveryCommandsRegister::moduleName() const
{
    return "projectrecovery";
}

const std::vector<Command>& ProjectRecoveryCommandsRegister::commandList() const
{
    static const std::vector<Command> commands = { PROJECT_RECOVERY_COMMAND };
    return commands;
}

const std::vector<CommandInfo>& ProjectRecoveryCommandsRegister::commandInfoList() const
{
    static const std::vector<CommandInfo> infos = {
        CommandInfo{
            PROJECT_RECOVERY_COMMAND,
            //: Command title: shown as a menu item or a button label; keep it short
            TranslatableString("command", "&Project recovery…"),
            //: Command description: shown as a tooltip; can be a full sentence
            TranslatableString("command_description", "Recover a corrupted project"),
            InputSchema(),
            Decoration()
        },
    };
    return infos;
}
