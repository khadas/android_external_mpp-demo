#!/system/bin/sh

H264="7"
JPEG="8"
H265="16777220"

#decode parameter
dec_video_360p="{i=/data/640x360.h264:w=640:h=360:t=${H264}:p=1:q=1}"
dec_video_720p="{i=/data/1280x720.h264:w=1280:h=720:t=${H264}:p=1:q=1}"
dec_video_1080p="{i=/data/1920x1080.h264:w=1920:h=1080:t=${H264}:p=1:q=1}"
dec_video_4k="{i=/data/4096x2304.h264:w=4096:h=2304:t=${H264}:p=1:q=1}"

#decode parameter(save yuv file)
dec_video_360p_save="{i=/data/640x480.h264:o=/data/640x360.h264.yuv:w=640:h=360:t=${H264}:p=3:q=1}"
dec_video_720p_save="{i=/data/1280x720.h264:o=/data/1280x720.h264.yuv:w=1280:h=720:t=${H264}:p=1:q=1}"
dec_video_1080p_save="{i=/data/1920x1080.h264:o=/data/1920x1080.h264.yuv:w=1920:h=1080:t=${H264}:p=1:q=1}"
dec_video_4k_save="{i=/data/4096x2304.h264:o=/data/4096x2304.h264.yuv:w=4096:h=2304:t=${H264}:p=1:q=1}"

#encode parameter
enc_video_480p="{i=/data/640x480.yuv:w=640:h=480:f=4:n=100:t=${H264}:p=1:q=2:r=1}"
enc_video_720p="{i=/data/1280x720.yuv:w=1280:h=720:f=4:n=100:t=${H264}:p=1:q=2:r=1}"
enc_video_1080p="{i=/data/1920x1080.yuv:w=1920:h=1080:f=4:n=100:t=${H264}:p=1:q=2:r=1}"

#encode parameter(save h264 file)
enc_video_480p_save="{i=/data/640x480.yuv:o=/data/640x480.yuv.h264:w=640:h=480:f=4:n=100:t=${H264}:p=1:q=2:r=1}"
enc_video_720p_save="{i=/data/1280x720.yuv:o=/data/1280x720.yuv.h264:w=1280:h=720:f=4:n=100:t=${H264}:p=1:q=2:r=1}"
enc_video_1080p_save="{i=/data/1920x1080.yuv:o=/data/1920x1080.yuv.h264:w=1920:h=1080:f=4:n=100:t=${H264}:p=1:q=2:r=1}"

function set_freq() {
    # if userspace doesn't exit, create it.
    if [ ! -d "/sys/class/devfreq/dmc/userspace/" ];then
        echo userspace > /sys/class/devfreq/dmc/governor
    fi

    # set cur_freq as params
    echo 800000000 > /sys/class/devfreq/dmc/userspace/set_freq
    cat /sys/class/devfreq/dmc/cur_freq
    
    echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
    echo userspace > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
    echo 1416000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed
    echo 1800000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_setspeed
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
    cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq
}

function decode_bench() {
    #排除IO，不保存文件
    mpi_multi_test ${dec_video_360p} ${dec_video_720p} ${dec_video_1080p}
}

function decode_4K_bench() {
    #排除IO，不保存文件
    mpi_multi_test ${dec_video_4k}
}

function decode_bench_save_file() {
		#保存文件
		mpi_multi_test ${dec_video_360p_save} ${dec_video_720p_save} ${dec_video_1080p_save}
		rm -rf /data/640x360.h264.yuv*
		rm -rf /data/1280x720.h264.yuv*
		rm -rf /data/1920x1080.h264.yuv*
}

function encode_bench() {
    #排除IO，不保存文件
    mpi_multi_test ${enc_video_480p} ${enc_video_720p} ${enc_video_1080p}
}

function encode_bench_save_file() {
    #保存文件
    mpi_multi_test ${enc_video_480p_save} ${enc_video_720p_save} ${enc_video_1080p_save}
    rm -rf /data/640x480.yuv.h264*
    rm -rf /data/1280x720.yuv.h264*
    rm -rf /data/1920x1080.yuv.h264*
}

function mpi_test_bench() {
		mpi_multi_test ${dec_video_360p} ${dec_video_720p} ${dec_video_1080p} ${enc_video_1080p}
}

function print_help() {
    echo "Please input Parameter:"
    echo "./mpp_test.sh 1	  ---decode_bench is not save yuv file"
    echo "./mpp_test.sh 2	  ---decode_bench_save_file is save yuv file"
    echo "./mpp_test.sh 3	  ---encode_bench is not save yuv file"
    echo "./mpp_test.sh 4	  ---encode_bench_save_file is save h264 file"
    echo "./mpp_test.sh 5	  ---decode_4K_bench is not save yuv file"
    echo "./mpp_test.sh 6	  ---mpi_test_bench is all-around test"
}

set_freq
if [ $# -lt 1 ]
then
		print_help
else
		if [ $1 -eq 1 ]
		then    
			decode_bench  
		elif [ $1 -eq 2 ]
		then
			decode_bench_save_file
		elif [ $1 -eq 3 ]
		then
			encode_bench
		elif [ $1 -eq 4 ]
		then
			encode_bench_save_file
		elif [ $1 -eq 5 ]
		then
			decode_4K_bench
		elif [ $1 -eq 6 ]
		then
			mpi_test_bench
		fi 
fi
