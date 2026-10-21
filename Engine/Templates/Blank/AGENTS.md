# Project workflow

This directory is an HEngine project. Project.heproject is its identity.
Use the engine installation recorded in HEngine.workspace.local.json.
Run hectl.bat here; no repeated -Project argument is needed. From an engine
checkout, use hectl project use -Project <this directory> to bind that workspace.
Use hectl project current -Json to inspect the selected project and output roots.
Engine changes belong to Engine/ and Tools/ in that engine checkout. Gameplay,
scenes, settings and assets belong here. Do not copy engine content into the project.
Generated build caches and Agent evidence live in the engine's out/agent, isolated
by project identity and path. Project Saved/ and Dist/ are generated local data.
Use bounded game check / editor capture for Agent validation; open is for users.

When EngineRoot contains HEngine.distribution.json, it is a binary installation.
Read its AGENTS.md and Docs/ReleaseSDK-1.1.0.md before selecting a workflow.
If the installation includes DevelopmentTemplates/Windows-x64/Template.json,
use hectl agent start -BuildProfile windows, discover the supported commands,
validate gameplay with input/frame steps/captures, and stop the owned session in
finally. This compiles project scripts and uses a precompiled development host.
Source-oriented game check/editor capture do not become available merely because
hectl is installed. Report missing capabilities; never silently change a game
into an engine fork.
