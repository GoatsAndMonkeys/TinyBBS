#!/usr/bin/env python3
"""
gen_survival_packed.py — Generate packed survival guide for external flash.

Output: data/survival.bin

Binary format:
  magic       uint32  0x53555256 ("SURV")
  cat_count   uint16  number of categories
  tip_count   uint16  total number of tips
  index[cat_count]:
    name        char[16]  category name
    first_tip   uint16    index of first tip
    num_tips    uint16    number of tips in category
  tips[tip_count]:
    len         uint8     length of tip text
    text        char[len] tip text
"""

import os
import struct
import sys

MAGIC = 0x53555256

# Full survival guide — sourced from FM 21-76, SAS Handbook, Red Cross
TIPS = {
    "Water": [
        "Morning dew collects on grass/metal. Wipe with cloth, wring into container. Can gather 1+ liter/hr at dawn.",
        "Solar still: dig 3ft hole, place container in center, cover with plastic sheet, weight center with rock. Yields ~1L/day.",
        "Boil water at rolling boil for 1 minute (3 min above 6500ft). Most reliable field purification method.",
        "Filter through layered sand/charcoal/gravel in a cut bottle, THEN boil. Filtering alone won't kill viruses.",
        "Follow animal tracks downhill at dawn/dusk - they lead to water. Birds flying low and straight at dusk head toward water.",
        "Green vegetation in dry areas signals subsurface water. Dig at the lowest point of the green area.",
        "Rain catchment: spread tarp/poncho angled into container. In tropics a single storm can yield 10+ liters.",
        "Never drink urine, seawater, or blood - all accelerate dehydration. Seawater causes kidney failure.",
        "2 drops unscented household bleach (6%) per liter, wait 30 min. Water should have faint chlorine smell.",
        "You can survive ~3 days without water. Ration sweat not water - rest in shade, move at night in hot climates.",
    ],
    "Fire": [
        "Fire triangle: heat + fuel + oxygen. Remove any one and fire dies. Build fires where wind feeds but doesn't scatter.",
        "Tinder first: dry grass, birch bark, cattail fluff, dryer lint. Then pencil-thin kindling, then thumb-thick fuel.",
        "Ferro rod works wet. Scrape coating off first strike. Aim sparks DOWN into tinder bundle. Works at any altitude.",
        "Bow drill: socket, spindle, fireboard, bow. Spindle on fireboard notch, bow rapidly. Coal forms in notch.",
        "Wet weather: split wood to expose dry interior. Use feather sticks (thin curls shaved on stick). Dead standing wood is driest.",
        "Fire signal: 3 fires in a triangle = international distress. Add green branches for white smoke visible for miles.",
        "Battery + steel wool or gum wrapper: touch both terminals with thin conductor. Short circuit creates ignition.",
        "Keep fire overnight: bank coals with ash, add a large green log. Uncover and add tinder in morning to restart.",
        "Platform fire for wet/snow ground: lay a base of green logs, build fire on top. Prevents fire from sinking.",
    ],
    "Shelter": [
        "Rule of 3s: 3 hours without shelter in harsh conditions can kill. Shelter is priority #1 before water or food.",
        "Lean-to: ridgepole between two trees, branches at 45deg on one side. Layer leaves/debris 2+ feet thick for waterproofing.",
        "Debris hut: body-length ridgepole, ribs on both sides, pile leaves/debris 3ft thick. Small entrance retains body heat.",
        "Snow cave: dig into packed drift, carve sleeping platform ABOVE entrance. Cold air sinks out. Poke ventilation hole in roof.",
        "Desert shelter: dig trench body-length, cover with tarp/brush 1ft above. Air gap insulates. Rest in shade during day.",
        "Ground insulation critical - you lose heat 25x faster to cold ground than cold air. Layer pine boughs/leaves 6in deep.",
        "Build small: body heat warms a small shelter. Just big enough to lie down. Smaller = warmer. Block entrance with debris.",
        "Site selection: avoid hilltops (wind), valley floors (cold air pools), dry riverbeds (flash floods). Use leeward mid-slopes.",
        "Poncho shelter: tie corners to trees, angle downhill. Even a trash bag split open can serve as emergency rain cover.",
    ],
    "FirstAid": [
        "Severe bleeding: apply direct pressure with cloth. If soaking through, pack wound tightly and press harder. Don't remove first bandage.",
        "Tourniquet for life-threatening limb bleeding: 2-3in above wound, tighten until bleeding stops. Note time applied.",
        "Burns: cool with clean running water 10+ minutes. Do NOT use ice, butter, or toothpaste. Cover loosely with sterile dressing.",
        "Splint fractures: immobilize joint above and below break. Pad with clothing. Check circulation below the splint.",
        "CPR: 30 chest compressions (2in deep, 100-120/min) then 2 rescue breaths. Push hard, push fast, allow full recoil.",
        "Shock: lay victim flat, elevate legs 12in, maintain body heat. Do NOT give fluids if abdominal injury suspected.",
        "Hypothermia: remove wet clothing, insulate from ground, warm core first (armpits, neck, groin). Warm sweet drinks if conscious.",
        "Heat stroke (hot/red/dry skin, confused): cool aggressively with ice on neck/armpits/groin, wet sheets, fan.",
        "Wound infection signs: increasing redness, swelling, warmth, red streaks, pus, fever. Clean wounds daily and re-dress.",
    ],
    "Navigation": [
        "Compass: red needle points magnetic north. Account for declination (difference between true and magnetic north) for your area.",
        "No compass? Stick shadow method: mark shadow tip, wait 15 min, mark again. Line between marks runs east-west.",
        "North Star (Polaris): find Big Dipper, extend two pointer stars 5x their distance. Polaris marks true north within 1 degree.",
        "Southern hemisphere: Southern Cross long axis x4.5 toward horizon = approximate south celestial pole.",
        "Analog watch compass: point hour hand at sun. Halfway between hour hand and 12 is roughly south (N. hemisphere).",
        "Follow water downstream - streams lead to rivers, rivers to civilization. But stay on ridges if terrain is too rugged.",
        "Moss grows on ALL sides of trees - not reliable. Use sun, stars, or prevailing wind patterns instead.",
        "Track pace count: average ~65 double-paces per 100m on flat ground. Adjust for terrain to estimate distance.",
        "Sun rises roughly east, sets roughly west. At solar noon, shadows point true north (northern hemisphere).",
    ],
    "Signaling": [
        "Mirror signal visible 50+ miles. Aim reflection at aircraft using V-sight of two fingers. Flash in sweeps.",
        "3 of anything = international distress: 3 fires, 3 shots, 3 whistle blasts, 3 flashes. Repeat at intervals.",
        "Ground-to-air symbols: stomp/build 10ft+ tall. V=need help. X=need medical. Arrow=traveling this direction.",
        "Whistle carries farther than voice, uses less energy. 3 short blasts = distress. Repeat every few minutes.",
        "Signal fire: keep dry tinder ready. Add rubber, oil, or green boughs for thick smoke when aircraft spotted.",
        "Bright clothing/tarp spread in clearing visible from air. Contrast with ground - orange on green, dark on snow.",
        "At night, fire is your best signal. On a hill or clearing, visible for miles. Flash light in groups of 3.",
        "Stay near crash/vehicle if possible. Larger object is easier to spot from air. Leave only if rescue is unlikely.",
    ],
    "Food": [
        "Rule of 3s: survive ~3 weeks without food. Prioritize water and shelter first. Don't waste calories foraging blindly.",
        "Universal edibility test: rub on skin (8hr), lip (15min), tongue (15min), chew/spit (15min), small amount (8hr).",
        "Insects: grasshoppers, crickets, ants, larvae are high-protein. Remove legs/wings, cook to kill parasites.",
        "Avoid: mushrooms (too risky without expert ID), milky sap plants, white/yellow berries, umbrella flower clusters.",
        "Fishing without hooks: thorns, safety pins, bone slivers work. Use insects, worms, or bright cloth as bait.",
        "Simple snare: noose of wire/cordage on small game trail. Anchor to stake. Set 6+ snares to improve odds.",
        "Dandelion: entire plant edible (leaves, root, flower). Found worldwide in temperate zones. Boil to reduce bitterness.",
        "Cattail: shoots, roots, and pollen all edible. Found near water worldwide. One of the best wild food sources.",
        "Cook all wild meat thoroughly - parasites are the real danger. Boiling preserves most nutrients (drink the broth).",
    ],
    "Radio": [
        "Meshtastic: 915MHz LoRa (US), no license needed. Long range mesh, text+GPS. Ideal off-grid group comms.",
        "Set one Meshtastic channel as emergency with known PSK. All group members relay messages even without line of sight.",
        "FRS channel 1 is common calling freq. No license needed. Range ~1 mile typical, 2+ with elevation.",
        "GMRS repeaters extend range to 20+ miles. Requires FCC license ($35/10yr). Channel 20 common for road/emergency.",
        "146.520 MHz = 2m ham calling frequency. Most monitored simplex freq. Use in emergency even without license.",
        "Ham distress: call MAYDAY 3x on 146.520 VHF or 14.300 MHz HF. Anyone may transmit in life-threatening emergency.",
        "NOAA Weather Radio: 162.400-162.550 MHz. Continuous broadcasts. Seven channels.",
        "Conserve radio battery: transmit briefly, listen more. Lower power extends battery. Carry spares or solar charger.",
        "CB radio channel 9 = emergency. Channel 19 = trucker channel near highways. No license needed, ~3 mile range.",
    ],
    "Weather": [
        "Cumulus clouds (puffy/white) = fair weather. When they tower vertically (cumulonimbus), storms likely within hours.",
        "Red sky at night = fair weather. Red sky at morning = storms approaching. Weather generally moves west to east.",
        "Falling barometric pressure = incoming storm. Rising altitude reading on altimeter without moving means pressure dropping.",
        "Wind shifting clockwise (N hemisphere) = improving. Counterclockwise = deteriorating weather.",
        "Thin halo around sun/moon = cirrostratus clouds = rain/snow likely in 12-24 hours.",
        "Sudden temp drop + dark clouds + gusty wind = severe weather imminent. Seek shelter, avoid ridges and lone trees.",
        "Morning fog burning off = fair weather. Fog thickening through day = rain likely.",
        "Insects flying low, birds roosting early, frogs loud often precede rain. Animals sense pressure changes.",
        "Lightning distance: count seconds between flash and thunder, divide by 5 = miles. Under 6 seconds = seek shelter NOW.",
    ],
    "Knots": [
        "Bowline: fixed loop that won't slip or jam. Rabbit out of hole, around tree, back in hole. Rescue essential.",
        "Clove hitch: quick attachment to post/tree. Two loops over object. Easy to tie/untie but can slip under variable load.",
        "Taut-line hitch: adjustable tension for guy lines and tarps. Slides to tighten, holds under load. Essential for shelters.",
        "Figure-eight: stopper knot that won't jam. Used at rope ends to prevent slipping through. Basis for climbing knots.",
        "Square/reef knot: joins two ropes of equal diameter. Right over left, then left over right. NOT for critical loads.",
        "Sheet bend: joins two ropes of DIFFERENT diameter. More secure than square knot. Double sheet bend for slippery rope.",
        "Prusik hitch: slides when unloaded, grips when loaded. Used to ascend rope or create adjustable anchors.",
        "Trucker's hitch: 3:1 mechanical advantage for tightening lines. Ideal for securing loads, tarps, ridgelines.",
    ],
    "Tools": [
        "Knife safety: cut away from body, keep sharp (dull knives slip). Use baton (striking stick) to split wood.",
        "No knife? Break rocks for sharp flakes. Flint, obsidian, quartz fracture into cutting edges. Strike at 30deg with hammerstone.",
        "Improvised cordage: inner bark of willow/basswood, yucca fibers, or twist green grass. Reverse-wrap method strongest.",
        "Digging stick: sharpen hardwood branch, fire-harden tip. For roots, grubs, shelter building, primitive trapping.",
        "Paracord: 550 cord has 7 inner strands ~50lbs each. Fishing line, snare wire, sewing thread, dental floss.",
        "Duct tape: repairs gear, closes wounds as butterfly strips, makes cordage, patches shelters. Wrap some around water bottle.",
        "Charred cloth (charcloth): char cotton in tin with small hole. Catches sparks from ferro rod or flint instantly.",
        "Improvised saw: rough-edged rock, or twist wire/paracord back and forth on wood. Slower but works for larger logs.",
    ],
    "Psychology": [
        "STOP when lost: Stop, Think, Observe, Plan. Panic causes bad decisions. Sit down, breathe, assess before acting.",
        "Survival priorities: 3 min air, 3 hrs shelter (extreme), 3 days water, 3 weeks food. Address in order.",
        "Set small achievable goals: gather wood, build shelter, find water. Completing tasks builds confidence.",
        "Maintain routine: wake, eat, work, rest at regular times. Structure prevents despair, gives sense of control.",
        "Talk to yourself or keep journal. Verbalizing plans improves decisions and reduces panic. Not weakness.",
        "Accept the situation. Denial wastes energy and time. Faster you accept reality, sooner you start solving problems.",
        "Inventory everything you have. People in survival often forget useful items. Check ALL pockets, gear, surroundings.",
        "Fear is normal and useful - increases alertness. Panic is fear without direction. Channel fear into action with a plan.",
        "Buddy system: if with others, assign tasks, check on each other. Groups survive better than individuals.",
        "Visualize rescue and reunion. Positive visualization measurably improves perseverance in survival situations.",
    ],
}


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    output_path = os.path.join(script_dir, "..", "data", "survival.bin")
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    categories = list(TIPS.keys())
    all_tips = []
    cat_entries = []

    for cat in categories:
        tips = TIPS[cat]
        # Validate all tips are under 200 bytes
        for tip in tips:
            encoded = tip.encode("utf-8")
            if len(encoded) > 199:
                print(f"WARNING: tip too long ({len(encoded)} bytes) in {cat}: {tip[:50]}...")
                # Truncate
                encoded = encoded[:199]
            all_tips.append(encoded)

        cat_entries.append({
            "name": cat,
            "first_tip": len(all_tips) - len(tips),
            "num_tips": len(tips),
        })

    tip_count = len(all_tips)
    cat_count = len(categories)

    with open(output_path, "wb") as f:
        # Header
        f.write(struct.pack("<IHH", MAGIC, cat_count, tip_count))

        # Index
        for entry in cat_entries:
            name = entry["name"].encode("ascii")[:15].ljust(16, b"\x00")
            f.write(name)
            f.write(struct.pack("<HH", entry["first_tip"], entry["num_tips"]))

        # Tips
        for tip in all_tips:
            f.write(struct.pack("<B", len(tip)))
            f.write(tip)

    size = os.path.getsize(output_path)
    print(f"Generated {output_path}")
    print(f"  {cat_count} categories, {tip_count} tips")
    print(f"  {size} bytes ({size/1024:.1f} KB)")
    for entry in cat_entries:
        print(f"  {entry['name']}: {entry['num_tips']} tips")


if __name__ == "__main__":
    main()
