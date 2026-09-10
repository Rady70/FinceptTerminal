set(_quoted_bait "
project(FinceptTerminal VERSION 9.9.9 LANGUAGES C CXX)
set_target_properties(FinceptTerminal PROPERTIES OUTPUT_NAME \"ForgedBinary\")
install(FILES packaging/linux/fincept-terminal.desktop DESTINATION applications)
")

project(FinceptTerminal VERSION 0.1.0 LANGUAGES C CXX)
set_target_properties(FinceptTerminal PROPERTIES OUTPUT_NAME "MarketLabTerminal")
install(FILES packaging/linux/fincept-terminal.desktop DESTINATION applications)
