<Qucs Schematic 26.1.2>
<Properties>
  <View=0,-60,1240,960,1,0,0>
  <Grid=10,10,1>
  <DataSet=RC_lowpass_ngsweep.dat>
  <DataDisplay=RC_lowpass_ngsweep.dpl>
  <OpenDisplay=0>
  <Script=RC_lowpass_ngsweep.m>
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
  <Vac V1 1 60 200 18 -26 1 1 "1 V" 1 "1 kHz" 0 "0" 0 "0" 0>
  <GND * 1 60 260 0 0 0 0>
  <R R1 1 160 140 -26 15 0 0 "1k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>
  <C C1 1 260 200 17 -26 0 1 "100n" 1 "" 0 "neutral" 0>
  <GND * 1 260 260 0 0 0 0>
  <.AC AC1 0 60 330 0 45 0 0 "log" 1 "10 Hz" 1 "100 kHz" 1 "81" 1 "no" 0>
  <.NGSWEEP NgSweep1 1 300 330 0 44 0 0 "Analysis=AC1" 1 "Param=R1" 1 "Type=log" 1 "Start=250" 1 "Stop=4k" 1 "Points=5" 1 "List=" 0 "Waveforms=yes" 0 "Record=fc|1/(2*pi*@r1[resistance]*@c1[capacitance])" 1>
</Components>
<Wires>
  <60 230 60 260 "" 0 0 0 "">
  <60 140 60 170 "" 0 0 0 "">
  <60 140 130 140 "in" 70 110 0 "">
  <190 140 260 140 "out" 230 110 0 "">
  <260 140 260 170 "" 0 0 0 "">
  <260 230 260 260 "" 0 0 0 "">
</Wires>
<Diagrams>
  <Rect 700 330 500 320 3 #c0c0c0 1 11 1 10 1 100000 1 0.01 1 1 1 -1 0.5 1 315 0 225 1 0 0 3 -1 "frequency (Hz)" "|v(out)| (V)" "">
	<"ngspice/ngsweep1.v(out)" #0000ff 2 3 0 0 0 1>
  </Rect>
  <Rect 700 760 500 300 3 #c0c0c0 1 11 1 250 1 4000 1 100 1 1000 1 -1 0.5 1 315 0 225 1 0 0 0 -1 "R1 (Ohm)" "corner frequency (Hz)" "">
	<"ngspice/ngsweep1.fc" #2a78d6 2 3 0 5 0>
  </Rect>
</Diagrams>
<Paintings>
  <Text 40 540 12 #000000 0 "An RC low-pass for five values of R1, 250 Ohm to 4 kOhm: NgSweep1 runs\nngspice's sweep command, which changes R1 and runs AC1 at each value.\nEvery value's v(out) is a curve of its own in the diagram above right; its\ngraph's color is auto, so each curve has a color of its own and the legend\nnames its R1. AC1 itself is switched off: only its sweep runs. NgSweep1 also\nrecords the corner frequency 1/(2 pi R1 C1) at every value (below right).\nDouble-click NgSweep1 for its form and the command it writes, or a graph\nfor its color; press F2. Needs an ngspice built with the sweep command\n(Ngspice-OpenVAF-Enhancements).">
</Paintings>
