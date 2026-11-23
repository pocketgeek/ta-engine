# The game room has no GAME MODS: ON/OFF option: multiplayer works like single
# player (the host ticks mods/game packs in the room). Run:
#   cmake -DLOBBY=<src/client/gameview_lobby.cpp> -DROOM=<src/client/gameview_room.cpp>
#         -P no_game_mods_toggle.cmake
file(READ "${LOBBY}" text)
file(READ "${ROOM}" room)
foreach(needle "\"GAME MODS: ON\"" "createGameMods")
  foreach(file LOBBY ROOM)
    if(file STREQUAL "LOBBY")
      string(FIND "${text}" "${needle}" at)
    else()
      string(FIND "${room}" "${needle}" at)
    endif()
    if(NOT at EQUAL -1)
      message(FATAL_ERROR "${${file}} has a GAME MODS on/off option (${needle})")
    endif()
  endforeach()
endforeach()
# The 64-pack limit counts the always-enabled packs, and still allows 64 in all
# (the room's mods screen, drawModsScreen).
string(FIND "${text}" "selection.size() + n + 1 <= 64" at)
if(at EQUAL -1)
  message(FATAL_ERROR "${LOBBY} lacks the game pack list rule (selection.size() + n + 1 <= 64)")
endif()
# A click on the GAME list keeps a saved tick that an always-enabled namesake hides
# (the remembered set the click writes, rememberRoomPrefs).
foreach(needle "case RoomField::Mods: {" "if (std::find(selection.begin(), selection.end(), n) == selection.end() &&")
  string(FIND "${room}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${ROOM} lacks the game pack list rule (${needle})")
  endif()
endforeach()
