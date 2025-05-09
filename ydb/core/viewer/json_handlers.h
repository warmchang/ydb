#pragma once
#include "viewer.h"
#include <ydb/core/viewer/json/json.h>
#include <ydb/core/viewer/yaml/yaml.h>

namespace NKikimr::NViewer {

class TJsonHandlerBase {
public:
    virtual ~TJsonHandlerBase() = default;

    virtual IActor* CreateRequestActor(IViewer* /* viewer */, NMon::TEvHttpInfo::TPtr& /* event */) {
        return nullptr;
    }

    virtual IActor* CreateRequestActor(IViewer* /* viewer */, NHttp::TEvHttpProxy::TEvHttpIncomingRequest::TPtr& /* event */) {
        return nullptr;
    }

    virtual bool IsMonEvent() const {
        return false;
    }

    virtual bool IsHttpEvent() const {
        return false;
    }

    virtual YAML::Node GetRequestSwagger() = 0;
};

template <typename ActorRequestType>
class TJsonHandler : public TJsonHandlerBase {
public:
    YAML::Node Swagger;

    TJsonHandler(YAML::Node swagger)
        : Swagger(swagger)
    {}

    IActor* CreateRequestActor(IViewer* viewer, NMon::TEvHttpInfo::TPtr& event) override {
        if constexpr (!std::is_same_v<ActorRequestType, void>) {
            return new ActorRequestType(viewer, event);
        } else {
            return nullptr;
        }
    }

    YAML::Node GetRequestSwagger() override {
        return Swagger;
    }

    bool IsMonEvent() const override {
        return true;
    }
};

template <typename ActorRequestType>
class THttpHandler : public TJsonHandlerBase {
public:
    YAML::Node Swagger;

    THttpHandler(YAML::Node swagger = {})
        : Swagger(swagger)
    {}

    IActor* CreateRequestActor(IViewer* viewer, NHttp::TEvHttpProxy::TEvHttpIncomingRequest::TPtr& event) override {
        return new ActorRequestType(viewer, event);
    }

    YAML::Node GetRequestSwagger() override {
        return Swagger;
    }

    bool IsHttpEvent() const override {
        return true;
    }
};

struct TJsonHandlers {
    std::vector<TString> JsonHandlersList;
    THashMap<TString, std::shared_ptr<TJsonHandlerBase>> JsonHandlersIndex;
    std::map<TString, int> Capabilities;

    void AddHandler(const TString& name, TJsonHandlerBase* handler, int version = 1) {
        JsonHandlersList.push_back(name);
        JsonHandlersIndex[name] = std::shared_ptr<TJsonHandlerBase>(handler);
        Capabilities[name] = version;
    }

    TJsonHandlerBase* FindHandler(const TString& name) const {
        auto it = JsonHandlersIndex.find(name);
        if (it == JsonHandlersIndex.end()) {
            return nullptr;
        }
        return it->second.get();
    }

    int GetCapabilityVersion(const TString& name) const {
        auto it = Capabilities.find(name);
        if (it == Capabilities.end()) {
            return 0;
        }
        return it->second;
    }
};

class TSimpleYamlBuilder {
public:
    struct TInitializer {
        TStringBuf Method;
        TStringBuf Tag;
        TStringBuf Url;
        TStringBuf Summary;
        TStringBuf Description;
    };

    struct TParameter {
        TStringBuf Name;
        TStringBuf Description;
        TStringBuf Type;
        TStringBuf Default;
        bool Required = false;
    };

    YAML::Node Root;
    YAML::Node Method;

    TSimpleYamlBuilder(TInitializer initializer);
    void SetParameters(YAML::Node parameters);
    void AddParameter(TParameter parameter);
    void SetResponseSchema(YAML::Node schema);

    operator YAML::Node() {
        return Root;
    }
};

} // namespace NKikimr::NViewer
