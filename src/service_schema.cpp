#include "service_provider.hpp"
#include "service_contract_data.hpp"
#include <cmath>
#include <regex>
#include <stdexcept>

namespace audio::service {
namespace {
const Json& Schema() { static const Json schema=Json::parse(schemaData);return schema; }
size_t Characters(const std::string& s) {
    size_t count{};for(unsigned char c:s)if((c&0xc0)!=0x80)++count;return count;
}
bool Match(const Json& value,const Json& rule) {
    if(rule.is_boolean())return rule.get<bool>();
    if(rule.contains("$ref"))return Match(value,Schema().at(Json::json_pointer(rule.at("$ref").get<std::string>().substr(1))));
    if(rule.contains("const") && value!=rule.at("const"))return false;
    if(rule.contains("enum")) {bool found=false;for(const auto& item:rule.at("enum"))if(value==item)found=true;if(!found)return false;}
    if(rule.contains("oneOf")) {int found=0;for(const auto& child:rule.at("oneOf"))if(Match(value,child))++found;if(found!=1)return false;}
    if(rule.contains("anyOf")) {bool found=false;for(const auto& child:rule.at("anyOf"))if(Match(value,child))found=true;if(!found)return false;}
    if(rule.contains("allOf"))for(const auto& child:rule.at("allOf"))if(!Match(value,child))return false;
    if(rule.contains("if")) {
        auto branch=Match(value,rule.at("if"))?"then":"else";
        if(rule.contains(branch)&&!Match(value,rule.at(branch)))return false;
    }
    if(rule.contains("type")) {
        const auto type=rule.at("type");
        if((type=="object"&&!value.is_object()) || (type=="array"&&!value.is_array()) ||
           (type=="string"&&!value.is_string()) || (type=="boolean"&&!value.is_boolean()) ||
           (type=="null"&&!value.is_null()) || (type=="number"&&!value.is_number()) ||
           (type=="integer"&&(!value.is_number()||!std::isfinite(value.get<double>())||std::floor(value.get<double>())!=value.get<double>())))return false;
    }
    if(value.is_object()) {
        if(rule.contains("required"))for(const auto& key:rule.at("required"))if(!value.contains(key.get<std::string>()))return false;
        if(rule.contains("properties"))for(auto it=rule.at("properties").begin();it!=rule.at("properties").end();++it)
            if(value.contains(it.key())&&!Match(value.at(it.key()),it.value()))return false;
        if(rule.value("additionalProperties",true)==false)for(auto it=value.begin();it!=value.end();++it)
            if(!rule.at("properties").contains(it.key()))return false;
    }
    if(value.is_array()&&rule.contains("items"))for(const auto& item:value)if(!Match(item,rule.at("items")))return false;
    if(value.is_number()) {
        auto n=value.get<double>();if(!std::isfinite(n))return false;
        if(rule.contains("minimum")&&n<rule.at("minimum").get<double>())return false;
        if(rule.contains("maximum")&&n>rule.at("maximum").get<double>())return false;
    }
    if(value.is_string()) {
        const auto& s=value.get_ref<const std::string&>();auto chars=Characters(s);
        if(rule.contains("minLength")&&chars<rule.at("minLength").get<size_t>())return false;
        if(rule.contains("maxLength")&&chars>rule.at("maxLength").get<size_t>())return false;
        if(rule.contains("x-maxUtf8Bytes")&&s.size()>rule.at("x-maxUtf8Bytes").get<size_t>())return false;
        if(rule.contains("pattern")) {
            auto pattern=rule.at("pattern").get<std::string>();
            if(pattern.rfind("^(?:",0)==0) {
                // The adopted base64 pattern; avoid recursive regex execution
                // on 44 KiB pages (bounded linear scan instead).
                if(s.size()%4)return false;
                size_t padding=0;
                for(size_t i=0;i<s.size();++i) {
                    const unsigned char c=s[i];
                    if(c=='=') {if(i<s.size()-2||++padding>2)return false;}
                    else if(padding||!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'))return false;
                }
            } else if(!std::regex_search(s,std::regex(pattern)))return false;
        }
    }
    return true;
}
}
void Validate(const Json& value,const std::string& definition) {
    // dump also validates UTF-8 rather than normalizing or silently replacing it.
    (void)value.dump();
    if(!Match(value,Schema().at("$defs").at(definition)))throw std::invalid_argument("Invalid audio-system v1 "+definition);
}
}
