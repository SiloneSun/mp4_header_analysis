mkdir build
cd build
rm ./* -rf
cmake ..
make -j8


strip analysis_mp4
copy_file analysis_mp4 ../
copy_file analysis_mp4 ~/work/tftp/
copy_file analysis_mp4 /home/sunxilong/work/tftp/sx_work/buildTools/
