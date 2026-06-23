# Marine Chartplotter Specification Recommendations

## Overview

A minimal core application focused on chart display, chart set management, ownship plotting, touch-friendly operation, and a plugin system for communication, instruments, AIS, routing, GPX import/export, and other higher-level features.

## Specs

The following files document importnat features:

docs/nav_data_store.md    -  The navigation data store
docs/ais_target_store.md    - The AIS data store
docs/pluging_api.md     - The plugin API system

## Guidlines

This application is touch first. There are no right clicks, double clicks, or features that require a physical mouse or keyboard.

Scrolling and panning is handled by touch and drag motion.

side_menu.cpp is the main menu for user functions. In light theme it is White and blue with dark text. Other dialogs and windows should be styled to look and work similar.

ais_target_info_window.cpp is an example of an information dialog. It has a darker apeparance, and other information diaplogs should be styled similar.

## Instructions

Never commit and merge without my permission. I will tell you when to commit and merge, and I will push to orgin myself when appropriate. 

Never push to origin. I will always do that.

Build the vs2022-release configuration by default. Do not build the debug configuration unless I ask.

Only edit files in your workspace. Do not edit anything in my main branch.



