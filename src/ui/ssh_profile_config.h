#pragma once

#include <string>
#include <yaml-cpp/yaml.h>

namespace clashflux::ui {

struct SshProfileConfig {
    std::string server;
    int port = 22;
    std::string username;
    std::string password;
    std::string privateKey;
    std::string passphrase;
    std::string hostKey;
};

inline std::string MakeSshProfileYaml(const SshProfileConfig& config) {
    const std::string name = "SSH · " + config.server;
    YAML::Emitter yaml;
    yaml << YAML::BeginMap
         << YAML::Key << "proxies" << YAML::Value << YAML::BeginSeq << YAML::BeginMap
         << YAML::Key << "name" << YAML::Value << YAML::DoubleQuoted << name
         << YAML::Key << "type" << YAML::Value << "ssh"
         << YAML::Key << "server" << YAML::Value << YAML::DoubleQuoted << config.server
         << YAML::Key << "port" << YAML::Value << config.port
         << YAML::Key << "username" << YAML::Value << YAML::DoubleQuoted << config.username
         << YAML::Key << "host-key" << YAML::Value << YAML::BeginSeq
         << YAML::DoubleQuoted << config.hostKey << YAML::EndSeq;
    if (!config.privateKey.empty()) {
        yaml << YAML::Key << "private-key" << YAML::Value << YAML::DoubleQuoted << config.privateKey;
        if (!config.passphrase.empty())
            yaml << YAML::Key << "private-key-passphrase" << YAML::Value << YAML::DoubleQuoted << config.passphrase;
    } else {
        yaml << YAML::Key << "password" << YAML::Value << YAML::DoubleQuoted << config.password;
    }
    yaml << YAML::EndMap << YAML::EndSeq
         << YAML::Key << "proxy-groups" << YAML::Value << YAML::BeginSeq << YAML::BeginMap
         << YAML::Key << "name" << YAML::Value << "PROXY"
         << YAML::Key << "type" << YAML::Value << "select"
         << YAML::Key << "proxies" << YAML::Value << YAML::BeginSeq
         << YAML::DoubleQuoted << name << YAML::EndSeq << YAML::EndMap << YAML::EndSeq
         << YAML::Key << "rules" << YAML::Value << YAML::BeginSeq << "MATCH,PROXY" << YAML::EndSeq
         << YAML::EndMap;
    return yaml.good() ? std::string(yaml.c_str()) : std::string{};
}

} // namespace clashflux::ui
