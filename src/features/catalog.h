#pragma once
// Spawnable items for Mafia: Definitive Edition: script model names for vehicles (carpls) and
// weapons (InventoryAddWeapon). Identifiers from hurfy's Lua-Tools-MafiaDE data tables (see
// THIRD_PARTY.md); display names and grouping are ours. Rows are sorted by group.

#include <cstddef>
#include <span>
#include <string>

namespace cg::features {

struct CatalogItem {
  const char* group;
  const char* name;
  const char* id;
};

inline constexpr CatalogItem kVehicles[] = {
    {"Berkley", "Berkley 810", "berkley_810"},
    {"Bolt", "Bolt Ace", "bolt_ace"},
    {"Bolt", "Bolt Ace Pickup", "bolt_ace_pickup"},
    {"Bolt", "Bolt Cooler", "bolt_cooler"},
    {"Bolt", "Bolt Delivery", "bolt_delivery"},
    {"Bolt", "Bolt Delivery (Ambulance)", "bolt_delivery_amb"},
    {"Bolt", "Bolt Hearse", "bolt_hearse"},
    {"Bolt", "Bolt Mail", "bolt_mail"},
    {"Bolt", "Bolt Model B", "bolt_model_b"},
    {"Bolt", "Bolt Pickup", "bolt_pickup"},
    {"Bolt", "Bolt Truck", "bolt_truck"},
    {"Bolt", "Bolt V8", "bolt_v8"},
    {"Brubaker", "Brubaker Forte", "brubaker_forte"},
    {"Bulworth", "Bulworth Packhard", "bulworth_packhard"},
    {"Bulworth", "Bulworth Sentry", "bulworth_sentry"},
    {"Carrozella", "Carrozella C-Series", "carrozella_c_series"},
    {"Culver", "Culver Airmaster", "culver_airmaster"},
    {"Eckhart", "Eckhart Crusader", "eckhart_crusader"},
    {"Eckhart", "Eckhart Elite", "eckhart_elite"},
    {"Eckhart", "Eckhart Fletcher", "eckhart_fletcher"},
    {"Falconer", "Falconer Classic", "falconer_classic"},
    {"Hank", "Hank A", "hank_a"},
    {"Haverley", "Haverley Tomahawk (Police)", "haverley_tomahawk_p"},
    {"Houston", "Houston Coupe", "houston_coupe"},
    {"Lassiter", "Lassiter V16", "lassiter_v16"},
    {"Lassiter", "Lassiter V16 Apollyon", "lassiter_v16_appolyon"},
    {"Lassiter", "Lassiter V16 Roadster", "lassiter_v16_roadster"},
    {"Parry", "Parry Bus", "parry_bus"},
    {"Samson", "Samson Drifter", "samson_drifter"},
    {"Samson", "Samson Tanker", "samson_tanker"},
    {"Shubert", "Shubert E-Six", "shubert_e_six"},
    {"Shubert", "Shubert E-Six (Detective)", "shubert_e_six_det"},
    {"Shubert", "Shubert E-Six (Police)", "shubert_e_six_p"},
    {"Shubert", "Shubert E-Six (Taxi)", "shubert_e_six_taxi"},
    {"Shubert", "Shubert Frigate", "shubert_frigate"},
    {"Shubert", "Shubert Six", "shubert_six"},
    {"Shubert", "Shubert Six (Detective)", "shubert_six_det"},
    {"Shubert", "Shubert Six (Police)", "shubert_six_p"},
    {"Shubert", "Shubert Six (Taxi)", "shubert_six_taxi"},
    {"Smith", "Smith Moray", "smith_moray"},
    {"Smith", "Smith Thrower", "smith_thrower"},
    {"Smith", "Smith V12", "smith_v12"},
    {"Smith", "Smith V12 Chicago", "smith_v12_chicago"},
    {"Trautenberg", "Trautenberg Sport", "trautenberg_sport"},
    {"Motorcycles", "Moto Blackcats", "moto_blackcats"},
    {"Motorcycles", "Motorcycle 1935", "motorcycle_1935"},
    {"Race & concept", "Celeste Mark 5", "celeste_mark_5"},
    {"Race & concept", "Crazy Horse", "crazy_horse"},
    {"Race & concept", "Disorder", "disorder"},
    {"Race & concept", "Flame Spear", "flame_spear"},
    {"Race & concept", "Manta Prototype", "manta_prototype"},
    {"Race & concept", "Mutagen", "mutagen"},
    {"Race & concept", "Waybar Concept", "waybar_concept"},
    {"Special (may not spawn)", "Betting Office", "betting_office"},
    {"Special (may not spawn)", "Bolt Truck (scripted, farm)", "bolt_truck_scripted_farm"},
    {"Special (may not spawn)", "Box Trailer", "box_trailer"},
};

inline constexpr CatalogItem kWeapons[] = {
    {"Pistols", "Semi-Auto Pistol", "p_mast_a_v1"},
    {"Pistols", "Gold Pistol", "p_mast_a_v2"},
    {"Revolvers", "Service Revolver", "rev_alf_a_v1"},
    {"Revolvers", "Magnum Revolver", "rev_alf_b_v1"},
    {"Revolvers", "Pocket Revolver", "rev_mast_special_a_v1"},
    {"Revolvers", "Golden Pocket Revolver", "rev_mast_special_a_v2"},
    {"Shotguns", "Shotgun", "sg_barker_a_v1"},
    {"Shotguns", "Golden Shotgun", "sg_barker_a_v2"},
    {"Shotguns", "Lupara", "sg_lupara_a_v1"},
    {"Rifles", "Bolt-Action Rifle", "rif_mayw_a_v1"},
    {"Rifles", "Sniper Rifle", "sn_mayw_a_v1"},
    {"Submachine guns", "Tommy Gun", "smg_trench_a_v1"},
    {"Submachine guns", "Golden Tommy Gun", "smg_trench_a_v2"},
    {"Melee", "Baseball Bat", "wep_baseball_bat_a_v1"},
    {"Melee", "Knife", "wep_knife_a_v1"},
    {"Melee", "Brass Knuckles", "wep_knuckle_a_v1"},
    {"Melee", "Wooden Plank", "wep_wooden_plank_a_v1"},
    {"Melee", "Crowbar", "wep_crowbar_a_v1"},
    {"Melee", "Metal Pipe", "wep_metal_bar_a_v1"},
    {"Developer (may not work)", "Test Railgun", "test_railgun"},
};

// Full item list grouped under headers, with an optional filter box. Click selects (writes the id
// into `selected`); double-click or Enter on a row returns true ("use it now").
bool DrawCatalog(const char* id, std::span<const CatalogItem> items, char* selected, size_t selectedSize, char* filter, size_t filterSize,
                 float rows = 12.0f);
// Display name for an id ("" when the id is not in the list).
std::string CatalogName(std::span<const CatalogItem> items, const std::string& id);

}  // namespace cg::features
