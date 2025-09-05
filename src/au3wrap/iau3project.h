/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <string>
#include <memory>

#include "global/io/path.h"
#include "global/types/ret.h"
#include "global/async/notification.h"
#include "modularity/imoduleinterface.h"

namespace au::au3 {
//! NOTE It's exactly IAu3Project, not just IProject
class IAu3Project
{
public:

    virtual ~IAu3Project() = default;

    virtual void open() = 0;
    virtual muse::Ret load(const muse::io::path_t& filePath) = 0;
    virtual bool save(const muse::io::path_t& fileName) = 0;
    virtual void close() = 0;

    virtual std::string title() const = 0;

    // Save status management
    [[nodiscard]] virtual bool hasUnsavedChanges() const = 0;
    virtual void markAsSaved() = 0;
    [[nodiscard]] virtual bool isRecovered() const = 0;

    virtual muse::async::Notification projectChanged() const = 0;

    // internal
    virtual uintptr_t au3ProjectPtr() const = 0;
};

class IAu3ProjectCreator : MODULE_EXPORT_INTERFACE
{
    INTERFACE_ID(IAu3ProjectCreator)
public:
    virtual ~IAu3ProjectCreator() = default;

    virtual std::shared_ptr<IAu3Project> create() const = 0;
};
}
