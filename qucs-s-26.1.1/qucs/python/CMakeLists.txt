# python scripts

SET(BASICS
  parse_result.py
  parse_result_example.py
  rc_ac_sweep.dat
  rc_ac_sweep.net
)

INSTALL(FILES ${BASICS} DESTINATION share/${QUCS_NAME}/python/)
# The qucs module (a simulation's dataset read in Python), for a Python
# outside Qucs-S; inside, the program writes its own copy on the path.
INSTALL(FILES module/qucs.py DESTINATION share/${QUCS_NAME}/python/)

