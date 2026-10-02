<Qucs Schematic 26.1.5>
<Properties>
  <View=0,0,1100,1300,1,0,0>
  <Grid=10,10,1>
  <DataSet=PRBS_eye_diagram.dat>
  <DataDisplay=PRBS_eye_diagram.dpl>
  <OpenDisplay=0>
  <Script=PRBS_eye_diagram.m>
  <RunScript=0>
  <showFrame=0>
  <FrameText0=Title>
  <FrameText1=Drawn By:>
  <FrameText2=Date:>
  <FrameText3=Revision:>
</Properties>
<Symbol>
</Symbol>
<Components>
  <vPRBS V1 1 120 300 18 -26 0 1 "0 V" 1 "1 V" 1 "100 ps" 1 "0" 0 "15 ps" 0 "15 ps" 0 "7" 1 "" 0 "NRZ" 0>
  <vTRNOISE V2 1 120 210 18 -6 0 1 "15m" 1 "10p" 1 "0" 0 "0" 0 "0" 0 "0" 0 "0" 0>
  <GND * 1 120 360 0 0 0 0>
  <R Rs 1 280 180 -26 -47 0 0 "50" 1 "26.85" 0 "european" 0>
  <R R2 1 400 180 -26 -47 0 0 "25" 1 "26.85" 0 "european" 0>
  <C C1 1 460 210 17 -26 0 1 "0.5 pF" 1 "" 0 "neutral" 0>
  <GND * 1 460 270 0 0 0 0>
  <R R3 1 560 180 -26 -47 0 0 "25" 1 "26.85" 0 "european" 0>
  <C C2 1 620 210 17 -26 0 1 "0.5 pF" 1 "" 0 "neutral" 0>
  <GND * 1 620 270 0 0 0 0>
  <.TR TR1 1 740 140 0 51 0 0 "lin" 1 "0" 1 "60 ns" 1 "20001" 0 "Trapezoidal" 0 "2" 0 "1 ns" 0 "1e-16" 0 "150" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "26.85" 0 "1e-3" 0 "1e-6" 0 "1" 0>
</Components>
<Wires>
  <120 330 120 360 "" 0 0 0 "">
  <120 240 120 270 "" 0 0 0 "">
  <120 180 250 180 "" 0 0 0 "">
  <310 180 370 180 "tx" 330 220 30 "">
  <430 180 460 180 "" 0 0 0 "">
  <460 240 460 270 "" 0 0 0 "">
  <460 180 530 180 "" 0 0 0 "">
  <590 180 620 180 "rx" 600 140 20 "">
  <620 240 620 270 "" 0 0 0 "">
</Wires>
<Diagrams>
  <Eye 100 720 420 260 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 -1 0.5 1 315 0 225 1 0 0 0 -1 - 2 2e-09 2 - 0 1 0.3 0.2 "" "" "" "The received eye">
	<"ngspice/tran.v(rx)" #0000ff 1 3 0 0 0>
  </Eye>
  <Rect 100 1080 420 220 3 #c0c0c0 1 00 0 0 1e-09 5e-09 1 -0.1 0.5 1.1 1 -1 0.5 1 315 0 225 1 0 0 1 -1 "" "" "" "The bits sent and received">
	<"ngspice/tran.v(tx)" #c00000 1 3 0 0 0>
	<"ngspice/tran.v(rx)" #0000ff 1 3 0 0 0>
  </Rect>
</Diagrams>
<Paintings>
  <Text 40 1150 12 #000000 0 "A 10 Gb/s PRBS7 - V1, a V(PRBS) source - with 15 mV of noise (V2, V(TRNOISE)) through a\ntwo-pole RC channel. The eye diagram folds v(rx) a unit interval apart - V1's Tbit, the\nsource rx's bits come from - from 2 ns on: the settling at the start left out. Beside it,\nthe eye's height and width, the jitter, the levels, Q, and how many UIs go through the\nmask, the hexagon at the eye's centre. Double-click the diagram: Properties > Eye sets the\nunit interval, the UIs across, the levels, the threshold, density or traces, and the mask.\nFor PAM4, set V1's Coding to PAM4 (and its Order to 13: PRBS13Q) and the eye's Levels to\n4. Needs an ngspice built with the PRBS source (Ngspice-OpenVAF-Enhancements).">
</Paintings>
