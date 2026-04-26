# Solar Team Solaris BMS Version 4

## To Do List 

### Hardware Changes
1. Make the KiCad schematic more readable and understandable. Separate all ICs to their own sheets. Use hierarchical sheets. This applies to the whole project. 
2. Fix the reset button issue. The RST pin is pulled low and is connected to ground through a pull-down resistor. Make the BQ76952 reset with software.
3. Implement the sleep mode feature.
4. Connect the PACK pin to the positive terminal of the battery pack.
5. Fix the VC0 and VC1 connections.
6. Add temperature reading to the BMS.
7. Fix the 5V regulator circuit. The cell connection can be reduced to 13 cells.
8. Revise the R32 and GND connection.
9. Make the component naming system consistent.
10. Add an external balancing system.
11. Make the net name conventions consistent.

### Software Changes
1. Implement the voltage reading algorithm.
2. Implement the current reading algorithm.
3. Make a dedicated library for the BQ76952.
