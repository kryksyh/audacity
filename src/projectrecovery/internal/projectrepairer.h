/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <vector>

#include "framework/global/io/ifilesystem.h"
#include "framework/global/io/path.h"
#include "framework/global/modularity/ioc.h"
#include "framework/global/types/retval.h"

#include "projectinspector.h"

namespace au::projectrecovery {
enum class RepairMethod {
    //! The autosave is deleted, so the last save opens
    RestoreLastSave,
    //! Missing blocks become silence of the same length; an autosave becomes the project
    SilenceMissingBlocks,
};

//! Whether `method` can repair what `inspection` found
muse::Ret checkRepairable(RepairMethod method, const Inspection& inspection);

struct RepairReport {
    //! The blocks that are silence of the same length in the output
    std::vector<MissingBlock> silencedBlocks;
    //! The inspection of the output, which has no problems
    Inspection verification;
};

class ProjectRepairer
{
    muse::GlobalInject<muse::io::IFileSystem> fileSystem;

public:
    //! Writes the repaired project to `output`, which must not exist. Never writes to the original or to its -wal and -shm
    //! files. When the repair fails, no output stays.
    //! workDir: the folder for the copy of an original with -wal frames; empty means a folder in the temporary directory
    muse::RetVal<RepairReport> repair(RepairMethod method, const muse::io::path_t& original, const muse::io::path_t& output,
                                      const muse::io::path_t& workDir = {}) const;

private:
    muse::Ret copyToOutput(const muse::io::path_t& original, const muse::io::path_t& output, const muse::io::path_t& workDir) const;
    muse::Ret silenceMissingBlocks(const muse::io::path_t& output, const Inspection& inspection) const;
    muse::Ret promoteAutosave(const muse::io::path_t& output) const;
    muse::Ret deleteAutosave(const muse::io::path_t& output) const;
    void removeOutput(const muse::io::path_t& output) const;
};
}
