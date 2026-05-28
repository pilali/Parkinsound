Stepgate plugin LV2 plugin with modgui. 

To build localy :
git clone https://github.com/pilali/Parkinsound.git
cd Parkinsound
make -j4

To build with mod-plugin-builder (https://github.com/mod-audio/mod-plugin-builder) :
copy the content of plugins/package/parkinsound-stepgate in mod-plugin-builder/plugins/package/parkinsound-stepgate
Then run ./build my_platform parkinsound-stepgate
