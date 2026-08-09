#include "mod/HudTryerMod.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDieEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/utility/buffer_span.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/network/packet/ClientboundMapItemDataPacket.h"
#include "mc/network/packet/InventorySlotPacket.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/network/packet/SetTitlePacket.h"
#include "mc/network/packet/SetTitlePacketPayload.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/util/ColorFormat.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/MapItem.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/saveddata/maps/MapDecoration.h"
#include "mc/world/level/saveddata/maps/MapItemTrackedActor.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kFullBlock = "\xE2\x96\x88";

struct PaletteEntry {
    std::string_view format;
    char             code;
};

constexpr std::array<PaletteEntry, 28> kPalette{{
    {ColorFormat::BLACK, '0'},
    {ColorFormat::DARK_BLUE, '1'},
    {ColorFormat::DARK_GREEN, '2'},
    {ColorFormat::DARK_AQUA, '3'},
    {ColorFormat::DARK_RED, '4'},
    {ColorFormat::DARK_PURPLE, '5'},
    {ColorFormat::GOLD, '6'},
    {ColorFormat::GRAY, '7'},
    {ColorFormat::DARK_GRAY, '8'},
    {ColorFormat::BLUE, '9'},
    {ColorFormat::GREEN, 'a'},
    {ColorFormat::AQUA, 'b'},
    {ColorFormat::RED, 'c'},
    {ColorFormat::LIGHT_PURPLE, 'd'},
    {ColorFormat::YELLOW, 'e'},
    {ColorFormat::WHITE, 'f'},
    {ColorFormat::MINECOIN_GOLD, 'g'},
    {ColorFormat::MATERIAL_QUARTZ, 'h'},
    {ColorFormat::MATERIAL_IRON, 'i'},
    {ColorFormat::MATERIAL_NETHERITE, 'j'},
    {ColorFormat::MATERIAL_REDSTONE, 'm'},
    {ColorFormat::MATERIAL_COPPER, 'n'},
    {ColorFormat::MATERIAL_GOLD, 'p'},
    {ColorFormat::MATERIAL_EMERALD, 'q'},
    {ColorFormat::MATERIAL_DIAMOND, 's'},
    {ColorFormat::MATERIAL_LAPIS, 't'},
    {ColorFormat::MATERIAL_AMETHYST, 'u'},
    {ColorFormat::MATERIAL_RESIN, 'v'},
}};

enum class ActionbarTransport { Plain, TextObject };

enum class MapPattern { Checker, Gradient };

struct MapPreviewSession {
    int                                   fakeSlot;
    ActorUniqueID                         mapId;
    std::chrono::steady_clock::time_point expiresAt;
};

constexpr int kMapWidth          = 128;
constexpr int kMapHeight         = 128;
constexpr int kMapPreviewSeconds = 10;

std::unordered_map<std::string, MapPreviewSession> gMapPreviewSessions;

Player* getPlayerFromOrigin(CommandOrigin const& origin) {
    auto* entity = origin.getEntity();
    if (entity == nullptr || !entity->isType(ActorType::Player)) {
        return nullptr;
    }

    return static_cast<Player*>(entity); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
}

std::string sanitizeForLog(std::string text) {
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r') {
            ch = '|';
        }
    }
    constexpr std::size_t maxLogLength = 160;
    if (text.size() > maxLogLength) {
        std::size_t cut = maxLogLength;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        text.resize(cut);
        text.append("...");
    }
    return text;
}

void sendHudPacket(Player& player, SetTitlePacketPayload::TitleType type, std::string const& debugLabel) {
    auto& mod = hud_tryer::HudTryerMod::getInstance();
    mod.logInfo("[HUD tryer][PKT] target=" + player.getRealName() + " " + debugLabel);

    SetTitlePacket packet(type);
    player.sendNetworkPacket(packet);
}

void sendTitleTimes(Player& player, int fadeIn, int stay, int fadeOut) {
    auto& mod = hud_tryer::HudTryerMod::getInstance();
    mod.logInfo(
        "[HUD tryer][PKT] target=" + player.getRealName() + " type=Times fadeIn=" + std::to_string(fadeIn)
        + " stay=" + std::to_string(stay) + " fadeOut=" + std::to_string(fadeOut)
    );

    SetTitlePacket packet(fadeIn, stay, fadeOut);
    player.sendNetworkPacket(packet);
}

void sendHudTextPacket(
    Player&                          player,
    SetTitlePacketPayload::TitleType type,
    std::string const&               text,
    bool                             logPayload = true
) {
    if (logPayload) {
        auto& mod = hud_tryer::HudTryerMod::getInstance();
        mod.logInfo(
            "[HUD tryer][PKT] target=" + player.getRealName() + " type=" + std::to_string(static_cast<int>(type))
            + " text=" + sanitizeForLog(text)
        );
    }

    SetTitlePacket packet(type, text, std::nullopt);
    player.sendNetworkPacket(packet);
}

std::string wrapTextObject(std::string const& text) {
    nlohmann::json component;
    component["text"] = text;

    nlohmann::json payload;
    payload["rawtext"] = nlohmann::json::array();
    payload["rawtext"].push_back(std::move(component));
    return payload.dump();
}

void sendActionbarProbe(
    Player&                    player,
    std::string const&         text,
    ActionbarTransport         transport,
    std::string_view           probeName,
    int                        rows,
    int                        columns
) {
    auto const type = transport == ActionbarTransport::Plain
                        ? SetTitlePacketPayload::TitleType::Actionbar
                        : SetTitlePacketPayload::TitleType::ActionbarTextObject;
    auto const wireText = transport == ActionbarTransport::Plain ? text : wrapTextObject(text);
    auto const transportName = transport == ActionbarTransport::Plain ? "actionbar" : "textobject";

    hud_tryer::HudTryerMod::getInstance().logInfo(
        "[HUD tryer][PROBE] target=" + player.getRealName() + " name=" + std::string(probeName)
        + " transport=" + transportName + " rows=" + std::to_string(rows) + " cols=" + std::to_string(columns)
        + " newlines=" + std::to_string(rows - 1) + " text_bytes=" + std::to_string(text.size())
        + " wire_bytes=" + std::to_string(wireText.size())
    );
    sendHudTextPacket(player, type, wireText, false);
}

std::string buildPaletteText() {
    std::string text;
    text.reserve(512);
    for (std::size_t index = 0; index < kPalette.size(); ++index) {
        auto const& entry = kPalette[index];
        text.append(entry.format);
        text.append(kFullBlock);
        text.append(kFullBlock);
        text.append(ColorFormat::RESET);
        text.push_back(entry.code);
        text.push_back(' ');
        if ((index + 1) % 4 == 0 && index + 1 != kPalette.size()) {
            text.push_back('\n');
        }
    }
    text.append(ColorFormat::RESET);
    return text;
}

std::string buildMatrixText() {
    constexpr std::array<std::size_t, 8> gradient{{4, 12, 6, 14, 2, 10, 3, 11}};
    std::string                           text;
    text.reserve(2048);

    for (int row = 0; row < 12; ++row) {
        std::size_t activeColor = kPalette.size();
        for (int column = 0; column < 24; ++column) {
            bool        transparent = false;
            std::size_t colorIndex  = 0;

            if (column < 8) {
                colorIndex = ((column / 2 + row / 2) % 2 == 0) ? 12 : 9;
            } else if (column < 16) {
                colorIndex = gradient[static_cast<std::size_t>(column - 8)];
            } else {
                colorIndex  = 17 + (static_cast<std::size_t>(column - 16 + row) % 11);
                transparent = row >= 4 && row <= 7 && column >= 18 && column <= 21;
            }

            if (transparent) {
                if (activeColor != kPalette.size()) {
                    text.append(ColorFormat::RESET);
                    activeColor = kPalette.size();
                }
                text.push_back(' ');
                continue;
            }

            if (activeColor != colorIndex) {
                text.append(kPalette[colorIndex].format);
                activeColor = colorIndex;
            }
            text.append(kFullBlock);
        }
        text.append(ColorFormat::RESET);
        if (row != 11) {
            text.push_back('\n');
        }
    }
    return text;
}

void showPalette(Player& player, ActionbarTransport transport) {
    sendActionbarProbe(player, buildPaletteText(), transport, "palette", 7, 16);
}

void showMatrix(Player& player, ActionbarTransport transport) {
    sendActionbarProbe(player, buildMatrixText(), transport, "matrix", 12, 24);
}

constexpr uint makeArgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return 0xFF000000U | (static_cast<uint>(red) << 16U) | (static_cast<uint>(green) << 8U)
         | static_cast<uint>(blue);
}

std::vector<uint> buildMapPixels(MapPattern pattern) {
    std::vector<uint> pixels(static_cast<std::size_t>(kMapWidth * kMapHeight));
    for (int row = 0; row < kMapHeight; ++row) {
        for (int column = 0; column < kMapWidth; ++column) {
            uint color = 0;
            if (pattern == MapPattern::Checker) {
                bool const light = ((column / 16) + (row / 16)) % 2 == 0;
                color            = light ? makeArgb(255, 170, 0) : makeArgb(20, 45, 110);

                if (row < 8) {
                    color = makeArgb(220, 35, 35);
                } else if (row >= kMapHeight - 8) {
                    color = makeArgb(35, 90, 230);
                } else if (column < 8) {
                    color = makeArgb(35, 210, 80);
                } else if (column >= kMapWidth - 8) {
                    color = makeArgb(245, 235, 40);
                }
            } else {
                auto const x = static_cast<uint>(column * 255 / (kMapWidth - 1));
                auto const y = static_cast<uint>(row * 255 / (kMapHeight - 1));

                // Bilinear blend: top-left red, top-right green, bottom-left blue, bottom-right white.
                auto const red = static_cast<std::uint8_t>(
                    ((255U - x) * (255U - y) + x * y) / 255U
                );
                auto const green = static_cast<std::uint8_t>(
                    (x * (255U - y) + x * y) / 255U
                );
                auto const blue = static_cast<std::uint8_t>(
                    ((255U - x) * y + x * y) / 255U
                );
                color            = makeArgb(red, green, blue);
            }
            pixels[static_cast<std::size_t>(row * kMapWidth + column)] = color;
        }
    }
    return pixels;
}

void sendClientHeldItem(Player& player, int slot, ItemStack const& item) {
    InventorySlotPacket(ContainerID::Inventory, static_cast<uint>(slot), item).sendTo(player);
    MobEquipmentPacket(player.getRuntimeID(), item, slot, slot, ContainerID::Inventory).sendTo(player);
}

bool restoreMapPreview(Player& player) {
    auto const name = player.getRealName();
    auto       found = gMapPreviewSessions.find(name);
    if (found == gMapPreviewSessions.end()) {
        return false;
    }

    auto& inventory = player.getInventory();
    auto const fakeSlot = found->second.fakeSlot;
    InventorySlotPacket(
        ContainerID::Inventory,
        static_cast<uint>(fakeSlot),
        inventory.getItem(fakeSlot)
    ).sendTo(player);

    auto const selectedSlot = player.getSelectedItemSlot();
    MobEquipmentPacket(
        player.getRuntimeID(),
        inventory.getItem(selectedSlot),
        selectedSlot,
        selectedSlot,
        ContainerID::Inventory
    ).sendTo(player);

    hud_tryer::HudTryerMod::getInstance().logInfo(
        "[HUD tryer][MAP] restored target=" + name + " fake_slot=" + std::to_string(fakeSlot)
        + " selected_slot=" + std::to_string(selectedSlot) + " map_id=" + std::to_string(found->second.mapId.rawID)
    );
    gMapPreviewSessions.erase(found);
    return true;
}

void restoreAllMapPreviews(Level& level) {
    std::vector<std::string> playerNames;
    playerNames.reserve(gMapPreviewSessions.size());
    for (auto const& [playerName, session] : gMapPreviewSessions) {
        static_cast<void>(session);
        playerNames.push_back(playerName);
    }

    for (auto const& playerName : playerNames) {
        if (auto* player = level.getPlayer(playerName)) {
            restoreMapPreview(*player);
        } else {
            gMapPreviewSessions.erase(playerName);
        }
    }
}

void expireMapPreviews(Level& level) {
    auto const now = std::chrono::steady_clock::now();
    std::vector<std::string> previewsToRestore;
    for (auto const& [playerName, session] : gMapPreviewSessions) {
        auto* player = level.getPlayer(playerName);
        if (player == nullptr || session.expiresAt <= now || player->getSelectedItemSlot() != session.fakeSlot) {
            previewsToRestore.push_back(playerName);
        }
    }

    for (auto const& playerName : previewsToRestore) {
        if (auto* player = level.getPlayer(playerName)) {
            restoreMapPreview(*player);
        } else {
            gMapPreviewSessions.erase(playerName);
        }
    }
}

void showMapPreview(Player& player, MapPattern pattern) {
    restoreMapPreview(player);

    ActorUniqueID mapId = player.getLevel().getNewUniqueID();
    ItemStack     mapItem;
    mapItem.reinit(std::string_view{"minecraft:filled_map"}, 1, 0);
    if (mapItem.isNull()) {
        throw std::runtime_error("failed to create minecraft:filled_map");
    }

    auto mapTag = std::make_unique<CompoundTag>();
    (*mapTag)[MapItem::TAG_MAP_UUID()] = mapId.rawID;
    mapItem.setUserData(std::move(mapTag));
    mapItem.serverInitNetId();

    auto pixels = buildMapPixels(pattern);
    buffer_span<uint> pixelSpan;
    pixelSpan.mBegin = pixels.data();
    pixelSpan.mEnd   = pixels.data() + pixels.size();

    std::vector<std::pair<MapItemTrackedActor::UniqueId, std::shared_ptr<MapDecoration>>> decorations;
    BlockPos mapOrigin{0, 0, 0};
    ClientboundMapItemDataPacket mapPacket(
        mapId,
        static_cast<schar>(0),
        decorations,
        pixelSpan,
        0,
        0,
        kMapWidth,
        kMapHeight,
        player.getDimensionId(),
        true,
        mapOrigin
    );

    auto const slot       = player.getSelectedItemSlot();
    auto const playerName = player.getRealName();
    gMapPreviewSessions.insert_or_assign(
        playerName,
        MapPreviewSession{
            slot,
            mapId,
            std::chrono::steady_clock::now() + std::chrono::seconds(kMapPreviewSeconds)
        }
    );

    try {
        sendClientHeldItem(player, slot, mapItem);
        mapPacket.sendTo(player);
    } catch (...) {
        try {
            restoreMapPreview(player);
        } catch (...) {
            gMapPreviewSessions.erase(playerName);
        }
        throw;
    }

    auto const patternName = pattern == MapPattern::Checker ? "checker" : "gradient";
    hud_tryer::HudTryerMod::getInstance().logInfo(
        "[HUD tryer][MAP] target=" + playerName + " pattern=" + patternName
        + " map_id=" + std::to_string(mapId.rawID) + " slot=" + std::to_string(slot) + " pixels=16384"
    );
}

void showActionbar(Player& player) {
    sendHudTextPacket(player, SetTitlePacketPayload::TitleType::Actionbar, "HUD TRYER ACTIONBAR 1234567890");
}

void showSubtitle(Player& player) {
    sendTitleTimes(player, 10, 793000, 20);
    sendHudTextPacket(player, SetTitlePacketPayload::TitleType::Title, "HUD TRYER");
    sendHudTextPacket(player, SetTitlePacketPayload::TitleType::Subtitle, "SUBTITLE line-2 line-3");
}

void showTitle(Player& player) {
    sendTitleTimes(player, 10, 793000, 20);
    sendHudTextPacket(player, SetTitlePacketPayload::TitleType::Title, "HUD TRYER TITLE line-3");
}

void clearHud(Player& player) {
    sendHudPacket(player, SetTitlePacketPayload::TitleType::Clear, "type=Clear");
}

void resetHud(Player& player) {
    sendHudPacket(player, SetTitlePacketPayload::TitleType::Reset, "type=Reset");
}

void sendUsage(CommandOutput& output) {
    output.success(
        "Usage: /hudtry <actionbar|palette|matrix|maptest|mapclear|subtitle|title|all|clear|reset> [mode]"
    );
}

void registerHudTryCommand() {
    auto& command = ll::command::CommandRegistrar::getInstance(false).getOrCreateCommand(
        "hudtry",
        "Try Bedrock HUD layers: actionbar, subtitle, title.",
        CommandPermissionLevel::Any
    );

    command.overload().execute([](CommandOrigin const&, CommandOutput& output) { sendUsage(output); });

    command.overload()
        .text("actionbar")
        .execute([](CommandOrigin const& origin, CommandOutput& output) {
            auto* player = getPlayerFromOrigin(origin);
            if (player == nullptr) {
                output.error("This command can only be used by a player.");
                return;
            }

            hud_tryer::HudTryerMod::getInstance().logInfo("[HUD tryer] /hudtry actionbar by " + player->getRealName());
            showActionbar(*player);
            output.success("Sent actionbar test to {}.", player->getRealName());
        });

    command.overload().text("palette").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = getPlayerFromOrigin(origin);
        if (player == nullptr) {
            output.error("This command can only be used by a player.");
            return;
        }

        showPalette(*player, ActionbarTransport::Plain);
        output.success("Sent 28-color Actionbar palette to {}.", player->getRealName());
    });

    command.overload()
        .text("palette")
        .text("textobject")
        .execute([](CommandOrigin const& origin, CommandOutput& output) {
            auto* player = getPlayerFromOrigin(origin);
            if (player == nullptr) {
                output.error("This command can only be used by a player.");
                return;
            }

            showPalette(*player, ActionbarTransport::TextObject);
            output.success("Sent 28-color ActionbarTextObject palette to {}.", player->getRealName());
        });

    command.overload().text("matrix").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = getPlayerFromOrigin(origin);
        if (player == nullptr) {
            output.error("This command can only be used by a player.");
            return;
        }

        showMatrix(*player, ActionbarTransport::Plain);
        output.success("Sent 24x12 Actionbar matrix to {}.", player->getRealName());
    });

    command.overload()
        .text("matrix")
        .text("textobject")
        .execute([](CommandOrigin const& origin, CommandOutput& output) {
            auto* player = getPlayerFromOrigin(origin);
            if (player == nullptr) {
                output.error("This command can only be used by a player.");
                return;
            }

            showMatrix(*player, ActionbarTransport::TextObject);
            output.success("Sent 24x12 ActionbarTextObject matrix to {}.", player->getRealName());
        });

    command.overload()
        .text("maptest")
        .text("checker")
        .execute([](CommandOrigin const& origin, CommandOutput& output) {
            auto* player = getPlayerFromOrigin(origin);
            if (player == nullptr) {
                output.error("This command can only be used by a player.");
                return;
            }

            try {
                showMapPreview(*player, MapPattern::Checker);
                output.success(
                    "Sent client-only 128x128 checker map to {} for {} seconds.",
                    player->getRealName(),
                    kMapPreviewSeconds
                );
            } catch (std::exception const& error) {
                hud_tryer::HudTryerMod::getInstance().logInfo(
                    "[HUD tryer][MAP] checker failed target=" + player->getRealName() + " error=" + error.what()
                );
                output.error("Failed to send checker map: {}", error.what());
            }
        });

    command.overload()
        .text("maptest")
        .text("gradient")
        .execute([](CommandOrigin const& origin, CommandOutput& output) {
            auto* player = getPlayerFromOrigin(origin);
            if (player == nullptr) {
                output.error("This command can only be used by a player.");
                return;
            }

            try {
                showMapPreview(*player, MapPattern::Gradient);
                output.success(
                    "Sent client-only 128x128 gradient map to {} for {} seconds.",
                    player->getRealName(),
                    kMapPreviewSeconds
                );
            } catch (std::exception const& error) {
                hud_tryer::HudTryerMod::getInstance().logInfo(
                    "[HUD tryer][MAP] gradient failed target=" + player->getRealName() + " error=" + error.what()
                );
                output.error("Failed to send gradient map: {}", error.what());
            }
        });

    command.overload().text("mapclear").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = getPlayerFromOrigin(origin);
        if (player == nullptr) {
            output.error("This command can only be used by a player.");
            return;
        }

        if (restoreMapPreview(*player)) {
            output.success("Restored the client inventory view for {}.", player->getRealName());
        } else {
            output.success("No active map preview for {}.", player->getRealName());
        }
    });

    command.overload()
        .text("subtitle")
        .execute([](CommandOrigin const& origin, CommandOutput& output) {
            auto* player = getPlayerFromOrigin(origin);
            if (player == nullptr) {
                output.error("This command can only be used by a player.");
                return;
            }

            hud_tryer::HudTryerMod::getInstance().logInfo("[HUD tryer] /hudtry subtitle by " + player->getRealName());
            showSubtitle(*player);
            output.success("Sent subtitle test to {}.", player->getRealName());
        });

    command.overload().text("title").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = getPlayerFromOrigin(origin);
        if (player == nullptr) {
            output.error("This command can only be used by a player.");
            return;
        }

        hud_tryer::HudTryerMod::getInstance().logInfo("[HUD tryer] /hudtry title by " + player->getRealName());
        showTitle(*player);
        output.success("Sent title test to {}.", player->getRealName());
    });

    command.overload().text("all").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = getPlayerFromOrigin(origin);
        if (player == nullptr) {
            output.error("This command can only be used by a player.");
            return;
        }

        hud_tryer::HudTryerMod::getInstance().logInfo("[HUD tryer] /hudtry all by " + player->getRealName());
        showActionbar(*player);
        showSubtitle(*player);
        showTitle(*player);
        output.success("Sent all HUD tests to {}.", player->getRealName());
    });

    command.overload().text("clear").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = getPlayerFromOrigin(origin);
        if (player == nullptr) {
            output.error("This command can only be used by a player.");
            return;
        }

        hud_tryer::HudTryerMod::getInstance().logInfo("[HUD tryer] /hudtry clear by " + player->getRealName());
        clearHud(*player);
        output.success("Cleared HUD text for {}.", player->getRealName());
    });

    command.overload().text("reset").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = getPlayerFromOrigin(origin);
        if (player == nullptr) {
            output.error("This command can only be used by a player.");
            return;
        }

        hud_tryer::HudTryerMod::getInstance().logInfo("[HUD tryer] /hudtry reset by " + player->getRealName());
        resetHud(*player);
        output.success("Reset HUD state for {}.", player->getRealName());
    });
}

} // namespace

namespace hud_tryer {

HudTryerMod& HudTryerMod::getInstance() {
    static HudTryerMod instance;
    return instance;
}

void HudTryerMod::logInfo(std::string const& message) const { getSelf().getLogger().info(message); }

bool HudTryerMod::load() {
    getSelf().getLogger().debug("Loading...");
    logInfo("[HUD tryer] load()");
    return true;
}

bool HudTryerMod::enable() {
    getSelf().getLogger().debug("Enabling...");
    logInfo("[HUD tryer] enable() registering /hudtry");
    registerHudTryCommand();

    auto& eventBus = ll::event::EventBus::getInstance();
    mLevelTickListener = eventBus.emplaceListener<ll::event::ServerLevelTickEvent>(
        [](ll::event::ServerLevelTickEvent& event) { expireMapPreviews(event.level()); }
    );
    mPlayerDisconnectListener = eventBus.emplaceListener<ll::event::PlayerDisconnectEvent>(
        [](ll::event::PlayerDisconnectEvent& event) {
            auto const erased = gMapPreviewSessions.erase(event.self().getRealName());
            if (erased > 0) {
                HudTryerMod::getInstance().logInfo(
                    "[HUD tryer][MAP] dropped disconnected session target=" + event.self().getRealName()
                );
            }
        }
    );
    mPlayerDieListener = eventBus.emplaceListener<ll::event::PlayerDieEvent>(
        [](ll::event::PlayerDieEvent& event) {
            auto const erased = gMapPreviewSessions.erase(event.self().getRealName());
            if (erased > 0) {
                HudTryerMod::getInstance().logInfo(
                    "[HUD tryer][MAP] dropped dead player session target=" + event.self().getRealName()
                );
            }
        }
    );
    return true;
}

bool HudTryerMod::disable() {
    getSelf().getLogger().debug("Disabling...");

    ll::service::getLevel().transform([](Level& level) {
        restoreAllMapPreviews(level);
        return true;
    });
    gMapPreviewSessions.clear();

    auto& eventBus = ll::event::EventBus::getInstance();
    if (mLevelTickListener) {
        eventBus.removeListener(mLevelTickListener);
        mLevelTickListener = nullptr;
    }
    if (mPlayerDisconnectListener) {
        eventBus.removeListener(mPlayerDisconnectListener);
        mPlayerDisconnectListener = nullptr;
    }
    if (mPlayerDieListener) {
        eventBus.removeListener(mPlayerDieListener);
        mPlayerDieListener = nullptr;
    }

    logInfo("[HUD tryer] disable()");
    return true;
}

} // namespace hud_tryer

LL_REGISTER_MOD(hud_tryer::HudTryerMod, hud_tryer::HudTryerMod::getInstance());
