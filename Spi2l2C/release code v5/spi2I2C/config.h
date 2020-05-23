//Uncomment this line to make the in-game temp readouts display in Fahrenheit.
//#define USE_FAHRENHEIT 1 

// this is where you can configure the addresses in HEX format
#define OLEDPRIMARY    0x3C 
#define OLEDSECONDARY    0x3D 

// this is where you setup your mono/dual screen config. valid choice are : 
// "us2066" for newheaven display oled character display or US2066 compatible oleds. 
// "NONE" for disabling the Secondary screen 
#define mainScreenType "us2066"
#define secScreenType "us2066" 

//this is where you can enable the left screen scroling.
//its to combat the oled burn in pixel effect that could happen over time 
#define primaryBurnIn   "enable"
#define secondaryBurnIn "enable"
