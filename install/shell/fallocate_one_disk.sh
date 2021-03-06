#!/bin/bash

mount_dir=/jovision/mnt
index_file_name=index
file_count_name=file_count

disk=$1
record_info_length=$2
frag_info_length=$3

disk_size=`parted /dev/$disk unit MiB print | grep -A2 Number | grep -v Number | grep -v '^$' | sed "s/MiB//g" | awk '{printf("%d\n", $4)}'`

if [ ${disk_size} -lt 102400 ];then
    disk_use_size=$(echo "${disk_size} - 1024" | bc)
else
    disk_use_size=$(echo "${disk_size} - 10240" | bc)
fi

echo "$disk use size is ${disk_use_size}m"

file_count=$(echo "${disk_use_size} / 256" | bc)

i=0
file_name=""
while [ $i -lt ${file_count} ];
do
	file_name=$(printf 'record_%05d' $i)

	fallocate -l 256m ${mount_dir}/${disk}/${file_name}
	i=$(($i + 1))
done

echo "start to fallocate index file"
#fallocate index file
file_use_bytes=$(echo "${record_info_length} + 256 * ${frag_info_length}" | bc)
index_file_size=$(echo "${file_use_bytes} * ${file_count}" | bc)
echo "index file size is ${index_file_size}"
fallocate -l ${index_file_size} ${mount_dir}/${disk}/${index_file_name}

dd if=/dev/zero of=${mount_dir}/${disk}/${index_file_name} seek=0 bs=128 count=${file_count} conv=notrunc

echo ${file_count} > ${mount_dir}/${disk}/${file_count_name}

echo "end fallocate index file"

echo "${disk} fallocate last file: ${file_name}"

