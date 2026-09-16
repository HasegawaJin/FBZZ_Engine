/// @file    AudioBus.cpp
/// @brief   バス構成の既定値と正規化。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Engine/Audio/AudioBus.hpp>
#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_map>

namespace fbzz::audio {
namespace {

std::string LowerCopy(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

std::vector<BusDesc> DefaultBusLayout()
{
    // SE と Voice だけが残響を受ける。BGM と UI は空間に置かれた音ではないので素通し。
    return {
        BusDesc{ kMasterBusName, {},             1.0f, 1.0f, false },
        BusDesc{ "BGM",          kMasterBusName, 1.0f, 1.0f, false },
        BusDesc{ "SE",           kMasterBusName, 1.0f, 1.0f, true  },
        BusDesc{ "UI",           kMasterBusName, 1.0f, 1.0f, false },
        BusDesc{ "Voice",        kMasterBusName, 1.0f, 1.0f, true  },
    };
}

std::vector<BusDesc> NormalizeBusLayout(const std::vector<BusDesc>& descs)
{
    std::vector<BusDesc> unique;
    std::unordered_map<std::string, size_t> indexByName;

    // Master は必ず先頭に 1 本だけ存在させる。入力に無ければ既定値で補う。
    unique.push_back(BusDesc{ kMasterBusName, {}, 1.0f, 1.0f, false });
    indexByName[LowerCopy(kMasterBusName)] = 0;

    for (const BusDesc& desc : descs) {
        if (desc.name.empty()) continue;
        const std::string key = LowerCopy(desc.name);
        const auto it = indexByName.find(key);
        if (it != indexByName.end()) {
            BusDesc& existing = unique[it->second];
            existing.volume        = desc.volume;
            existing.lowPassCutoff = desc.lowPassCutoff;
            existing.reverb        = desc.reverb;
            if (it->second != 0) existing.parent = desc.parent;
            continue;
        }
        indexByName[key] = unique.size();
        unique.push_back(desc);
    }
    unique[0].parent.clear();

    // 親を先に出す順序へ並べ替える。親が未解決のまま一巡したら Master 直下へ倒す。
    // WHY 倒すか: 綴り違いや循環で 1 本でも構築に失敗すると、そのバスへ送る音が
    //     まるごと無音になる。設定ミスは「意図しない場所で鳴る」で済ませ、
    //     「原因の分からない無音」にはしない。
    std::vector<BusDesc> ordered;
    std::vector<bool> emitted(unique.size(), false);
    ordered.reserve(unique.size());
    ordered.push_back(unique[0]);
    emitted[0] = true;

    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (size_t i = 1; i < unique.size(); ++i) {
            if (emitted[i]) continue;
            const auto parent = indexByName.find(LowerCopy(unique[i].parent));
            if (parent == indexByName.end() || !emitted[parent->second]) continue;
            ordered.push_back(unique[i]);
            emitted[i] = true;
            progressed = true;
        }
    }
    for (size_t i = 1; i < unique.size(); ++i) {
        if (emitted[i]) continue;
        BusDesc fallback = unique[i];
        fallback.parent = kMasterBusName;
        ordered.push_back(std::move(fallback));
    }

    return ordered;
}

} // namespace fbzz::audio
