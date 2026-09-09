# MarketLab Terminal

MarketLab Terminal is a local-first financial research workstation derived from Fincept Terminal 4.5.0. It is intended for public-market data, local analytics, portfolio research, historical simulation, notes, reports, spreadsheets, and user-managed integrations without a Fincept account.

This fork does not include the upstream AI chat, LLM provider/model stack, agent frameworks, AI Quant Lab, Chat Mode, Alpha Arena, speech-assistant components, or their Python dependencies. Existing AI-related database rows from an older profile may remain inert; this release does not create, expose, synchronize, or delete them.

## Retained capabilities

- Public market, economic, government, news, and alternative-data connectors
- Watchlists, manually maintained portfolios, and equity research
- Local analytics, derivatives tools, and historical backtesting
- Notes, report builder, file manager, code editor, spreadsheet, and visual workflows
- Generic MCP server management and tool execution for user-managed local integrations
- Local SQLite persistence, cache, logging, Python runner, and network safety guards

External broker and exchange order routes are unavailable in this fork. Backtesting remains historical simulation and does not grant paper or live brokerage authority.

## Build from source

The supported Windows developer preset uses CMake, Ninja, MSVC, Qt 6.8.x, and Python 3.11:

```powershell
cd fincept-qt
cmake --preset win-dev -DFINCEPT_BUILD_TESTS=ON
cmake --build --preset win-dev
ctest --test-dir build/win-dev --output-on-failure
```

Run the hosted-path audit independently with:

```powershell
python -X utf8 marketlab/audit_hosted_paths.py
```

The built application is `fincept-qt/build/win-dev/MarketLabTerminal.exe`.

## Project boundaries

- No Fincept-hosted API, cloud-sync, billing, subscription, forum, support, or update path is treated as available.
- Public and user-configured third-party data sources retain their own terms, credentials, availability, and quality limits.
- Missing, stale, delayed, failed, and zero data are distinct states; successful software execution is not evidence of profitability or current data quality.
- Do not commit credentials, account identifiers, positions, balances, or runtime databases.

See the companion `Market_Lab` repository for the fork plan, qualification evidence, architecture disposition, and roadmap.

## License and attribution

The upstream project is Fincept Terminal by Fincept Corporation. This fork preserves the repository's AGPL-3.0 license; see [LICENSE](LICENSE).
