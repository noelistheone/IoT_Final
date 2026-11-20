#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace aircue {
constexpr size_t max_wifi_profiles=8;
struct WifiProfile {
    std::string ssid,password;
    bool valid() const {
        return !ssid.empty() && ssid.size()<=32 && ssid.find('\0')==std::string::npos &&
            password.find('\0')==std::string::npos && (password.empty() || (password.size()>=8 && password.size()<=63));
    }
    bool operator==(const WifiProfile& other) const {return ssid==other.ssid && password==other.password;}
    bool operator!=(const WifiProfile& other) const {return !(*this==other);}
};
inline bool remember_wifi(std::vector<WifiProfile>& profiles,const WifiProfile& profile,bool prefer=false) {
    if(!profile.valid()) return false;
    auto it=std::find_if(profiles.begin(),profiles.end(),[&](const WifiProfile& p){return p.ssid==profile.ssid;});
    if(it==profiles.end()) {
        if(profiles.size()>=max_wifi_profiles) return false;
        profiles.push_back(profile);it=profiles.end()-1;
    } else *it=profile;
    if(prefer) std::rotate(profiles.begin(),it,it+1);
    return true;
}
inline int wifi_profile_index(const std::vector<WifiProfile>& profiles,const WifiProfile& profile) {
    auto it=std::find(profiles.begin(),profiles.end(),profile);
    return it==profiles.end()?-1:static_cast<int>(it-profiles.begin());
}
inline bool remove_wifi(std::vector<WifiProfile>& profiles,const std::string& ssid) {
    auto it=std::find_if(profiles.begin(),profiles.end(),[&](const WifiProfile& p){return p.ssid==ssid;});
    if(it==profiles.end()) return false;
    profiles.erase(it);return true;
}
inline bool prefer_wifi(std::vector<WifiProfile>& profiles,const std::string& ssid) {
    auto it=std::find_if(profiles.begin(),profiles.end(),[&](const WifiProfile& p){return p.ssid==ssid;});
    if(it==profiles.end()) return false;
    std::rotate(profiles.begin(),it,it+1);return true;
}
struct WifiRotation {
    size_t index=0,failed=0;
    uint32_t backoff_ms=1000;
    void reset() {index=failed=0;backoff_ms=1000;}
    void connected() {failed=0;backoff_ms=1000;}
    uint32_t failure(size_t count) {
        if(!count) return 60000;
        index=(index+1)%count;
        // Try every saved network before backing off; a bad password cannot starve a fallback.
        if(++failed<count) return 500;
        failed=0;uint32_t delay=backoff_ms;
        backoff_ms=std::min<uint32_t>(60000,backoff_ms*2);
        return delay;
    }
};
}
