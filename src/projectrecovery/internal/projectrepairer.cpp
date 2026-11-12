/*
 * Audacity: A Digital Audio Editor
 */
#include "projectrepairer.h"

#include <set>
#include <string>

#include "database.h"
#include "documentedit.h"
#include "workingcopy.h"

using namespace muse;
using namespace au::projectrecovery;

namespace {
constexpr const char* SIDE_FILES[] = { "-wal", "-shm" };

std::string problemText(Problem problem)
{
    switch (problem) {
    case Problem::DatabaseNotReadable:
        return "SQLite cannot read the file";
    case Problem::NotAudacityProject:
        return "the file is not an Audacity project";
    case Problem::WalNotApplied:
        return "the -wal file cannot be applied";
    case Problem::NewerProjectVersion:
        return "the file was saved by a newer version of Audacity";
    case Problem::NoDocument:
        return "the file has no project document";
    case Problem::DocumentNotReadable:
        return "the project document does not decode";
    case Problem::MissingBlocks:
        return "blocks are missing";
    case Problem::MissingBlockWithoutLength:
        return "a missing block has no length";
    case Problem::IntegrityErrors:
        return "the database has integrity errors";
    }
    return "an unknown problem";
}

//! Only missing blocks have a repair for now
Ret checkRepairable(const Inspection& inspection)
{
    std::string others;
    for (const Problem problem : inspection.problems) {
        if (problem != Problem::MissingBlocks) {
            others += (others.empty() ? "" : "; ") + problemText(problem);
        }
    }
    if (!others.empty()) {
        return make_ret(Ret::Code::NotSupported, "no repair for these problems: " + others);
    }
    if (!inspection.has(Problem::MissingBlocks)) {
        return make_ret(Ret::Code::BadArgs, "the project has no problem to repair");
    }
    return make_ok();
}
}

RetVal<RepairReport> ProjectRepairer::repair(const io::path_t& original, const io::path_t& output, const io::path_t& workDir) const
{
    // SQLite would apply a -wal file that it finds next to the output
    for (const char* suffix : { "", "-wal", "-shm" }) {
        if (fileSystem()->exists(output + suffix)) {
            return RetVal<RepairReport>::make_ret(make_ret(Ret::Code::BadArgs, "the file exists: " + (output + suffix).toStdString()));
        }
    }

    Ret ret = copyToOutput(original, output, workDir);
    if (!ret) {
        removeOutput(output);
        return RetVal<RepairReport>::make_ret(ret);
    }

    // The output has no -wal frames, so this inspection reads what the repair edits
    const RetVal<Inspection> inspection = ProjectInspector().inspect(output);
    ret = inspection.ret ? checkRepairable(inspection.val) : inspection.ret;
    if (ret) {
        ret = silenceMissingBlocks(output, inspection.val);
    }
    if (!ret) {
        removeOutput(output);
        return RetVal<RepairReport>::make_ret(ret);
    }

    const RetVal<Inspection> verification = ProjectInspector().inspect(output);
    if (!verification.ret || !verification.val.isHealthy()) {
        removeOutput(output);
        std::string problems = verification.ret ? std::string() : "; " + verification.ret.text();
        for (const Problem problem : verification.val.problems) {
            problems += "; " + problemText(problem);
        }
        return RetVal<RepairReport>::make_ret(make_ret(Ret::Code::InternalError,
                                                       "the output failed its check and was deleted" + problems));
    }

    RepairReport report;
    report.silencedBlocks = inspection.val.missingBlocks;
    report.verification = verification.val;
    return RetVal<RepairReport>::make_ok(report);
}

Ret ProjectRepairer::copyToOutput(const io::path_t& original, const io::path_t& output, const io::path_t& workDir) const
{
    WorkingCopy copy;
    Ret ret = copy.prepare(original, workDir);
    if (!ret) {
        return ret;
    }
    // Without wal applied the content of the file misses commited data
    if (copy.walSize() > 0 && !copy.walApplied()) {
        return make_ret(Ret::Code::NotSupported, "the -wal file cannot be applied: " + copy.walError());
    }
    ret = fileSystem()->copy(copy.path(), output);
    if (ret && copy.originalChanged()) {
        ret = make_ret(Ret::Code::Busy, "the project was changed by another program");
    }
    return ret;
}

Ret ProjectRepairer::silenceMissingBlocks(const io::path_t& output, const Inspection& inspection) const
{
    const std::string& table = inspection.activeDocument;
    if (table != "project" && table != "autosave") {
        return make_ret(Ret::Code::InternalError, "no document to repair");
    }
    std::set<int64_t> ids;
    for (const MissingBlock& block : inspection.missingBlocks) {
        ids.insert(block.id);
    }

    Database database;
    Ret ret = database.open(output, Database::Mode::ReadWrite);
    if (!ret) {
        return ret;
    }
    BinaryXml binary;
    bool found = false;
    ret = database.query("SELECT dict, doc FROM " + table + " WHERE id = 1;", {}, [&](const DatabaseRow& row) {
        binary = { row.blob(0), row.blob(1) };
        found = true;
    });
    if (!ret) {
        return ret;
    }
    if (!found) {
        return make_ret(Ret::Code::InternalError, "the output has no " + table + " document");
    }

    const RetVal<ByteArray> doc = silenceBlocks(binary, ids);
    if (!doc.ret) {
        return doc.ret;
    }
    ret = database.query("UPDATE " + table + " SET doc = ?1 WHERE id = 1;", { doc.val });
    if (!ret) {
        return ret;
    }
    database.close();

    // Audacity opens a project with a -wal file as one that did not close
    for (const char* suffix : SIDE_FILES) {
        if (fileSystem()->exists(output + suffix)) {
            return make_ret(Ret::Code::InternalError, std::string("the output keeps its ") + suffix + " file");
        }
    }
    return make_ok();
}

void ProjectRepairer::removeOutput(const io::path_t& output) const
{
    for (const char* suffix : { "", "-wal", "-shm" }) {
        if (fileSystem()->exists(output + suffix)) {
            fileSystem()->remove(output + suffix);
        }
    }
}
