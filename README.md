Stepgate plugin LV2 plugin with modgui. 

<img width="250" height="250" alt="screenshot-parkinsound-stepgate" src="https://github.com/user-attachments/assets/d1811acf-d645-4b58-b76b-488c935b438c" />

To build localy :

git clone https://github.com/pilali/Parkinsound.git

cd Parkinsound

make -j4

To build with mod-plugin-builder (https://github.com/mod-audio/mod-plugin-builder) :

copy the content of plugins/package/parkinsound-stepgate in mod-plugin-builder/plugins/package/parkinsound-stepgate

Then run ./build my_platform parkinsound-stepgate
