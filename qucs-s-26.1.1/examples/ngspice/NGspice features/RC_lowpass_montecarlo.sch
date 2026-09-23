<Qucs Schematic 26.1.2>
<Properties>
  <View=0,-60,1180,960,1,0,0>
  <Grid=10,10,1>
  <DataSet=RC_lowpass_montecarlo.dat>
  <DataDisplay=RC_lowpass_montecarlo.dpl>
  <OpenDisplay=0>
  <Script=RC_lowpass_montecarlo.m>
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
  <R R1 1 160 140 -26 15 0 0 "{rr}" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>
  <C C1 1 260 200 17 -26 0 1 "{cc}" 1 "" 0 "neutral" 0>
  <GND * 1 260 260 0 0 0 0>
  <SpicePar SpicePar1 1 60 330 -28 16 0 0 "rr=agauss(1k, 150, 3)" 1 "cc=agauss(100n, 15n, 3)" 1>
  <.AC AC1 1 60 430 0 45 0 0 "log" 1 "100 Hz" 1 "100 kHz" 1 "61" 1 "no" 0>
  <.NGMONTECARLO NgMonteCarlo1 1 300 330 0 44 0 0 "Samples=200" 1 "Seed=1" 0 "LHS=no" 0 "ModelStats=no" 0 "Analysis=AC1" 1 "Record=gain|db(v(out))" 1 "Record=fc|1/(2*pi*@r1[resistance]*@c1[capacitance])" 1 "Spec=1/(2*pi*@r1[resistance]*@c1[capacitance])|1.432k|1.751k" 1>
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
  <Rect 620 290 480 280 3 #c0c0c0 1 10 1 100 1 100000 1 -50 10 5 1 -1 0.5 1 315 0 225 0 0 0 "" "" "">
	<"ngspice/ngmontecarlo1.gain" #0000ff 0 3 0 0 0>
  </Rect>
  <Histogram 620 660 480 280 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 -1 0.5 1 315 0 225 0 0 0 0 -1 0 0 3 1432 1751 "" "" "">
	<"ngspice/ngmontecarlo1.fc" #0050c8 1 3 0 0 0>
  </Histogram>
</Diagrams>
<Paintings>
  <Text 40 620 12 #000000 0 "A 1.59 kHz RC low-pass with parts of 5 % (one sigma): SpicePar1 draws R1 and C1 from\nGaussians, agauss(nominal, three sigma, 3). NgMonteCarlo1 runs ngspice's montecarlo\ncommand: 200 samples of AC1, each with new values. It records the gain of every sample\n(a family of 200 curves, above right) and the corner frequency 1/(2 pi R C) of each - in a\nhistogram diagram below, with the normal distribution of the same mean and deviation\nand the limits - and judges the corner frequency against 1591.5 Hz +/- 10 %: the yield\nand its 95 % confidence interval are in the status log, and in the dataset as\nngmontecarlo1.yield. Double-click NgMonteCarlo1 for its form and the command it\nwrites; press F2. Needs an ngspice built with montecarlo (Ngspice-OpenVAF-Enhancements).">
</Paintings>
