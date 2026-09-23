<Qucs Schematic 26.1.2>
<Properties>
  <View=0,-60,1180,760,1,0,0>
  <Grid=10,10,1>
  <DataSet=LC_lowpass_ngopt.dat>
  <DataDisplay=LC_lowpass_ngopt.dpl>
  <OpenDisplay=0>
  <Script=LC_lowpass_ngopt.m>
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
  <Vac V1 1 60 200 18 -26 1 1 "1 V" 1 "1 MHz" 0 "0" 0 "0" 0>
  <GND * 1 60 260 0 0 0 0>
  <R Rs 1 130 140 -26 15 0 0 "50 Ohm" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>
  <C C1 1 200 200 17 -26 0 1 "Cin" 1 "" 0 "neutral" 0>
  <GND * 1 200 260 0 0 0 0>
  <L L1 1 260 140 -26 10 0 0 "Lm" 1 "" 0>
  <C C2 1 320 200 17 -26 0 1 "Cout" 1 "" 0 "neutral" 0>
  <GND * 1 320 260 0 0 0 0>
  <R Rl 1 400 200 15 -26 0 1 "50 Ohm" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>
  <GND * 1 400 260 0 0 0 0>
  <.AC AC1 1 40 330 0 45 0 0 "log" 1 "100 kHz" 1 "10 MHz" 1 "41" 1 "no" 0>
  <Eqn Eqn1 1 250 340 -31 17 0 0 "Cin=2.2n" 1 "Lm=4.7u" 1 "Cout=2.2n" 1 "yes" 0>
  <NutmegEq NutmegEq1 1 250 450 -30 18 0 0 "AC1" 1 "gain=db(2*v(out))" 1>
  <.NGOPT NgOpt1 1 540 330 0 44 0 0 "Method=lm" 1 "MaxIter=" 0 "Tol=" 0 "Size=" 0 "Seed=" 0 "Verbose=no" 0 "Analysis=" 0 "Minimize=" 0 "Knob=dparam|Cin|2.2n|470p|22n" 1 "Knob=dparam|Lm|4.7u|1u|47u" 1 "Knob=dparam|Cout|2.2n|470p|22n" 1 "Target=ac lin 1 0.5meg 0.5meg|db(2*v(out))|-0.0673|" 1 "Target=ac lin 1 1meg 1meg|db(2*v(out))|-3.0103|" 1 "Target=ac lin 1 2meg 2meg|db(2*v(out))|-18.129|" 1>
</Components>
<Wires>
  <60 230 60 260 "" 0 0 0 "">
  <60 140 60 170 "" 0 0 0 "">
  <60 140 100 140 "in" 70 110 0 "">
  <160 140 200 140 "" 0 0 0 "">
  <200 140 200 170 "" 0 0 0 "">
  <200 230 200 260 "" 0 0 0 "">
  <200 140 230 140 "" 0 0 0 "">
  <290 140 320 140 "" 0 0 0 "">
  <320 140 320 170 "" 0 0 0 "">
  <320 230 320 260 "" 0 0 0 "">
  <320 140 400 140 "out" 370 110 0 "">
  <400 140 400 170 "" 0 0 0 "">
  <400 230 400 260 "" 0 0 0 "">
</Wires>
<Diagrams>
  <Rect 560 290 480 280 3 #c0c0c0 1 10 1 0 1e+06 1e+07 1 -60 10 5 1 -1 0.5 1 315 0 225 0 0 0 "" "" "">
	<"ngspice/ac.gain" #0000ff 2 3 0 0 0>
  </Rect>
</Diagrams>
<Paintings>
  <Text 40 580 12 #000000 0 "The same 50 Ohm LC low-pass, fitted by ngspice itself: NgOpt1 runs ngspice's optimize command\n(Levenberg-Marquardt least squares) on the three .param values of Eqn1, so that the gain matches a\n1 MHz Butterworth response at three frequencies: -0.0673 dB at 0.5 MHz, -3.0103 dB at 1 MHz and\n-18.129 dB at 2 MHz, each measured after a one-point AC analysis. The answer is 3.183 nF, 15.92 uH,\n3.183 nF. Double-click NgOpt1 for its form and the command it writes; press F2. ngspice leaves the\ncircuit at the optimum, so AC1 and the diagram show it, and the values found become the initial\nvalues of the knobs. Needs an ngspice built with optimize (Ngspice-OpenVAF-Enhancements).">
</Paintings>
