// Help topics an add-on adds (docs/EDITOR_SPEC.md 5.6; FINDINGS 770-776): Descriptions of the
// HelpEntry cast, each page a ControlCreator, placed in the base Help tree by anchor topics.
#pragma once

#include <string>
#include <vector>

#include "addon.h"

namespace ed {

struct HelpTopic {
	std::string path;     // topic names from the Help tree's root, '/'-separated
	std::string title;    // a heading above the page, in the robot pages' larger text
	std::string image;    // "picture" (the HAR's Choose Creature picture), "base:<Image name>" (one of OMFHelp's), or a PNG/BMP file
	std::string same_as;  // a base topic's path whose page this one shows instead of its own
	std::string text;     // the page, '\n' between lines
	bool left = false;    // lines at the left, as the generic pages; else centred, as the robot pages
	bool operator==(const HelpTopic &) const = default;
};

// "== path" starts a topic; "title:", "image:", "page:" and "align: left" lines may follow it before
// the text; '#' lines are comments. Throws deo::Error with the line of a mistake.
std::vector<HelpTopic> parse_help_file(const std::string &text);
std::string format_help_file(const std::vector<HelpTopic> &topics);

// The base Help tree (OMFHelp's HelpEntry descriptions) by path.
std::vector<std::string> base_help_paths(const BaseData &base);

// The add-on's topics, in file order; an anchor (a base topic the add-on's topics hang under) has
// same_as set to its own path. `pages` gets each topic's text Value\String (-1 for none).
std::vector<HelpTopic> read_help(const Project &p, std::vector<int> *pages = nullptr);

// Adds or updates each topic (by path). Path prefixes that are base topics get anchors; any other
// parent must come earlier in `topics` or the add-on. `picture` is the image "picture" names.
void write_help(Project &p, const std::vector<HelpTopic> &topics, int picture = -1);

}  // namespace ed
