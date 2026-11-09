/* The Online Tourney table's bracket made from the lobby's (GET /bracket.json, tools/bracket.py's
 * view), for one player: the lines hle_tourney_ui.c's parse_bracket takes. Plain C with nothing of the
 * runtime in it (hle_tourney_table.c), so tools/dev/tourneytabletest.c runs it on made-up views and
 * tests/test_bracket.py checks it against tools/bracket_tui.py, which makes the same in Python (its
 * docstring has the steps). */
#pragma once

/* The table's lines for player `me` from the view `js` (JSON) into out, or 0 when there is none to show
 * (or it does not fit). can_end: the table can move on past its last match (hle_tourney_ui.c's
 * table_show_match is hooked). */
int tourney_table(const char *js, const char *me, int can_end, char *out, int cap);
