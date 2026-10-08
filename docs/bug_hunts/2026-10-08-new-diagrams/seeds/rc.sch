<Qucs Schematic 26.1.7>
<Properties>
  <View=-2049,-566,4039,5726,0.105904,0,620>
  <Grid=10,10,1>
  <DataSet=RC_filter_FFT.dat>
  <DataDisplay=RC_filter_FFT.dpl>
  <OpenDisplay=0>
  <Script=RC filter FFT.m>
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
  <C C1 1 200 210 27 -23 0 1 "10nF" 1 "" 0 "neutral" 0>
  <GND * 1 200 260 0 0 0 0>
  <Vrect V1 1 80 210 -90 -49 1 1 "1 V" 1 ".5 ms" 1 ".5 ms" 1 "1 ns" 1 "1 ns" 1 "0 ns" 0 "0 V" 1>
  <GND * 1 80 260 0 0 0 0>
  <R R1 1 140 150 -28 -51 1 0 "100k" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "US" 0>
  <.TR TR1 1 -30 320 0 50 0 0 "lin" 1 "0" 1 "10 ms" 1 "1000" 1 "Trapezoidal" 0 "2" 0 "1 ns" 0 "1e-16" 0 "150" 0 "0.001" 0 "1 pA" 0 "1 uV" 0 "26.85" 0 "1e-3" 0 "1e-6" 0 "1" 0 "CroutLU" 0 "no" 0 "yes" 0 "0" 0>
  <.FFT FFT1 1 -30 510 0 50 0 0 "1MHz" 1 "1kHz" 1 "hanning" 1 "2" 0 "0" 0 "yes" 0>
  <NutmegEq NutmegEq1 1 0 690 -30 18 0 0 "fft" 1 "S=db(v(out))" 1>
</Components>
<Wires>
  <200 240 200 260 "" 0 0 0 "">
  <80 240 80 260 "" 0 0 0 "">
  <200 150 200 180 "" 0 0 0 "">
  <170 150 200 150 "" 0 0 0 "">
  <80 150 80 180 "" 0 0 0 "">
  <80 150 110 150 "in" 50 120 0 "">
  <200 150 200 150 "out" 210 120 0 "">
</Wires>
<Diagrams>
  <Rect 380 311 329 231 3 #c0c0c0 1 00 1 0 0.0002 0.001 1 -1.19998 1 1.19998 1 -8.49897 5 5 315 0 225 0 0 0 0 -1 "" "" "">
	<"ngspice/RC_filter_FFT:tran.v(in)" #0000ff 0 3 0 0 0>
	<"ngspice/RC_filter_FFT:tran.v(out)" #ff0000 0 3 0 0 1>
  </Rect>
  <Rect 374 704 574 284 3 #c0c0c0 1 00 1 0 1e+08 1e+09 0 -150 25 0 1 -1 0.5 1 315 0 225 0 0 0 0 -1 "" "" "">
	<"ngspice/RC_filter_FFT:ac.s" #ff0000 0 3 0 0 0>
  </Rect>
  <Stacked 20 1150 320 320 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 3 1 0 1 1 0 0 1 L 1 0 1 1 0 0 0 L 1 0 1 1 0 0 1 L 1 0 1 1 0 0 0 L 1 0 1 1 0 0 1 L 1 0 1 1 0 0 0 L "" "" "">
	<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>
	  <Mkr 0.002 84 -311 3 0 0>
	  <Mkr 0.00400001 148 -317 3 0 0 2 - - -1 1>
	<"ngspice/tran.v(in)" #ff0000 1 3 0 0 0 0 0 0 2>
	<Limit 0 0 0 "mask" 0,0.7;0.005,0.7;0.005,0.5;0.01,0.5>
	<Limit 1 0 2 "" 0,0.1>
  </Stacked>
  <Bode 20 1640 400 360 3 #c0c0c0 1 10 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 2 1 0 1 1 1 1 1 L 1 0 1 1 0 0 0 L 1 0 45 45 0 0 1 L 1 0 1 1 0 0 0 L 1 "" "" "">
	<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>
  </Bode>
  <Nichols 20 2070 360 300 3 #c0c0c0 1 00 1 -180 45 0 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 1 "" "" "">
	<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>
  </Nichols>
  <PoleZero 20 2500 300 300 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 1 "" "" "">
	<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>
  </PoleZero>
  <Spectrum 10 2890 400 260 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 5 9 1 1 - - "" "" "">
	<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>
  </Spectrum>
  <Bathtub 10 3280 400 260 3 #c0c0c0 1 01 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 0 0 0 0 -1 0.0005 - 0 - 1e-12 - 1 "" "" "">
	<"ngspice/tran.v(in)" #0000ff 1 3 0 0 0>
  </Bathtub>
  <Contour 10 3670 360 260 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 8 0 1 1 - - "" "" "">
	<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>
  </Contour>
  <Spectrogram 10 4060 360 260 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 0 1 1 0 - - 1 0.001 0.5 80 - "" "" "">
	<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>
  </Spectrogram>
  <Bars 10 4430 320 240 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 1 - 15 "" "" "">
	<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>
	<"ngspice/tran.v(in)" #ff0000 1 3 0 0 0>
  </Bars>
  <BoxPlot 0 4780 300 220 2 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 - 0 "" "" "">
	<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>
	<"ngspice/tran.v(in)" #ff0000 1 3 0 0 0>
  </BoxPlot>
  <Constellation 0 5170 260 260 3 #c0c0c0 1 00 1 -2 1 2 1 -2 1 2 1 0 1 1 315 0 225 1 0 0 0 -1 0.0005 - - 2 "" "" "">
	<"ngspice/tran.v(in)" #0000ff 1 3 0 0 0>
	<"ngspice/tran.v(out)" #ff0000 1 3 0 0 0>
  </Constellation>
  <Polar 0 5500 200 200 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 1 1 "" "" "">
	<"ngspice/tran.v(out)" #0000ff 1 3 0 0 0>
  </Polar>
</Diagrams>
<Paintings>
</Paintings>
