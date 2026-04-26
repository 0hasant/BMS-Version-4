Solar Team Solaris BMS Version 4

To do list 
 HARDWARE CHANGES
1. Fix the reset button issue.The RST pin is pullled low and is connected to ground through a pull-down resistor. Make the bq76952 resets with software.
2. Implement the sleep mode feature.
3. Connect the PACK pin to the positive terminal of the battery pack.
4. Fix the VCO and VC1 connections.
5. Add tepmrature reading to the bms.
6. Fix the 5V regulator circuit. The cell conection can be reduce to 13 cells.
7. Revise the R32 and GND  connection.
8. Make the companet naming system consistent.
9. Add a external ballancing system.
10. Make the net name conventions consistent.


SOFTWARE CHANGES
1. Implement the votlage reading algorithm.
2. Implement the current reading algorithm.
3. Make a dedicated library for the bq76952.


