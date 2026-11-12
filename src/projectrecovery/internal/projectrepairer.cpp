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
    case Problem::StaleAutosave:
        return "the autosave misses blocks that the last save does not need";
    case Problem::IntegrityErrors:
        return "the database has integrity errors";
    }
    return "an unknown problem";
}
}

Ret au::projectrecovery::checkRepairable(RepairMethod method, const Inspection& inspection)
{
    const bool restore = method == RepairMethod::RestoreLastSave;
    std::string others;
    for (const Problem problem : inspection.problems) {
        // Restoring the last save drops the autosave together with its block lengths
        const bool handled = problem == Problem::MissingBlocks || problem == Problem::StaleAutosave
                             || (restore && problem == Problem::MissingBlockWithoutLength);
        if (!handled) {
            others += (others.empty() ? "" : "; ") + problemText(problem);
        }
    }
    if (!others.empty()) {
        return make_ret(Ret::Code::NotSupported, "no repair for these problems: " + others);
    }
    const Problem needed = restore ? Problem::StaleAutosave : Problem::MissingBlocks;
    if (!inspection.has(needed)) {
        return make_ret(Ret::Code::BadArgs, "the project has no problem that this method repairs");
    }
    return make_ok();
}

RetVal<RepairReport> ProjectRepairer::repair(RepairMethod method, const io::path_t& original, const io::path_t& output,
                                             const io::path_t& workDir) const
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
    ret = inspection.ret ? checkRepairable(method, inspection.val) : inspection.ret;
    if (ret) {
        if (method == RepairMethod::RestoreLastSave) {
            ret = deleteAutosave(output);
        } else {
            ret = silenceMissingBlocks(output, inspection.val);
            if (ret && inspection.val.activeDocument == "autosave") {
                ret = promoteAutosave(output);
            }
        }
    }
    // Audacity opens a project with a -wal file as one that did not close
    for (const char* suffix : SIDE_FILES) {
        if (ret && fileSystem()->exists(output + suffix)) {
            ret = make_ret(Ret::Code::InternalError, std::string("the output keeps its ") + suffix + " file");
        }
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
    if (method == RepairMethod::SilenceMissingBlocks) {
        report.silencedBlocks = inspection.val.missingBlocks;
    }
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
    database.close();
    return ret;
}

Ret ProjectRepairer::promoteAutosave(const io::path_t& output) const
{
    Database database;
    Ret ret = database.open(output, Database::Mode::ReadWrite);
    if (!ret) {
        return ret;
    }
    // As ProjectFileIO::WriteDoc writes a save: the project row, then the same document in the journal of the last 10
    std::string sql = "BEGIN;"
                      "INSERT OR REPLACE INTO project(id, dict, doc) SELECT 1, dict, doc FROM autosave WHERE id = 1;";
    if (database.hasTable("project_history")) {
        sql += "INSERT INTO project_history(saved_at, dict, doc)"
               " SELECT CAST(strftime('%s','now') AS INTEGER), dict, doc FROM project WHERE id = 1;"
               "DELETE FROM project_history WHERE generation NOT IN"
               " (SELECT generation FROM project_history ORDER BY generation DESC LIMIT 10);";
    }
    sql += "DELETE FROM autosave;"
           "COMMIT;";
    ret = database.exec(sql);
    if (!ret) {
        database.exec("ROLLBACK;");
    }
    database.close();
    return ret;
}

Ret ProjectRepairer::deleteAutosave(const io::path_t& output) const
{
    Database database;
    Ret ret = database.open(output, Database::Mode::ReadWrite);
    if (ret) {
        ret = database.exec("DELETE FROM autosave;");
    }
    database.close();
    return ret;
}

void ProjectRepairer::removeOutput(const io::path_t& output) const
{
    for (const char* suffix : { "", "-wal", "-shm" }) {
        if (fileSystem()->exists(output + suffix)) {
            fileSystem()->remove(output + suffix);
        }
    }
}
