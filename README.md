# Solar Team Solaris BMS Version 4

## To Do List

### Hardware Changes

1. Make the KiCad schematic more readable and understandable. Separate all ICs to their own sheets. Use hierarchical sheets. This applies to the whole project.
2. Fix the reset button issue. The RST pin is pulled low and is connected to ground through a pull-down resistor. Make the BQ76952 reset with software. // 02.05.2026 - Done by Hasan Turan.  - Hardware reset used.
3. Implement the sleep mode feature.
4. Connect the PACK pin to the positive terminal of the battery pack. //Done by Hasan Turan
5. Fix the VC0 and VC1 connections. // Done by Hasan Turan.
6. Add temperature reading to the BMS.
7. Fix the 5V regulator circuit. The cell connection can be reduced to 13 cells.//Done by Hasan Turan
8. Revise the R32 and GND connection.// 02.05.2026 - Done by Hasan Turan.
9. Make the component naming system consistent.
10. Add an external balancing system.
11. Make the net name conventions consistent.
12. Place R18 cap to near.
13. Place the current read cap to near to BQ.
14. Add two switch for BMS numbering.

### Software Changes

1. Implement the voltage reading algorithm.// been doing by Hasan Turan.
2. Implement the current reading algorithm.// been doing by Hasan Turan.
3. Make a dedicated library for the BQ76952.// been doing by Hasan Turan.
