CLASS=acm
VID=0x3346
PID=0x1003
MSC_PID=0x1008
NCM_PID=0x100C
NCM_DEV_ADDR="02:00:00:00:20:02"
NCM_HOST_ADDR="02:00:00:00:20:01"
UVC_PID=0x100A
UAC_PID=0x100B
ADB_VID=0x18D1
ADB_PID=0x4EE0
ADB_PID_M1=0x4EE2
ADB_PID_M2=0x4EE4
MANUFACTURER="Cvitek"
PRODUCT="USB Com Port"
PRODUCT_NCM="NCM"
PRODUCT_UVC="UVC"
PRODUCT_UAC="UAC"
PRODUCT_ADB="ADB"
ADBD_PATH=/usr/bin/
SERIAL="0123456789"
MSC_FILE=$3
CVI_DIR=/sys/kernel/config
CVI_GADGET=$CVI_DIR/usb_gadget/cvitek
CVI_FUNC=$CVI_GADGET/functions
FUNC_NUM=0
MAX_EP_NUM=4
TMP_NUM=0
INTF_NUM=0
EP_IN=0
EP_OUT=0

set_ncm_addresses() {
	machine_hex=$(tr -cd '[:xdigit:]' </etc/machine-id 2>/dev/null | cut -c1-10)
	if [ "${#machine_hex}" -eq 10 ]; then
		mac_tail=$(printf '%s' "$machine_hex" | sed 's/^\(..\)\(..\)\(..\)\(..\)\(..\)$/\1:\2:\3:\4:\5/')
		NCM_DEV_ADDR="02:$mac_tail"
		NCM_HOST_ADDR="06:$mac_tail"
	fi
}

case "$2" in
  acm)
	CLASS=acm
	;;
  msc)
	CLASS=mass_storage
	PID=$MSC_PID
	;;
  cvg)
	CLASS=cvg
	;;
  ncm)
	CLASS=ncm
	PID=$NCM_PID
	PRODUCT=$PRODUCT_NCM
	set_ncm_addresses
	;;
  uvc)
	CLASS=uvc
	PID=$UVC_PID
	PRODUCT=$PRODUCT_UVC
	;;
  uac1)
	CLASS=uac1
	PID=$UAC_PID
	PRODUCT=$PRODUCT_UAC
	;;
  adb)
	CLASS=ffs.adb
	VID=$ADB_VID
	PID=$ADB_PID
	PRODUCT=$PRODUCT_ADB
	;;
  *)
	if [ "$1" = "probe" ] ; then
	  echo "Usage: $0 probe {acm|msc|cvg|ncm|uvc|uac1|adb}"
	  exit 1
	fi
esac

calc_func() {
  FUNC_NUM=$(ls $CVI_GADGET/functions -l | grep ^d | wc -l)
  echo "$FUNC_NUM file(s)"
}

res_check() {
  TMP_NUM=$(find $CVI_GADGET/functions/ -name "acm*" | wc -l)
  EP_OUT=$(($EP_OUT+$TMP_NUM))
  TMP_NUM=$(($TMP_NUM * 2))
  EP_IN=$(($EP_IN+$TMP_NUM))
  INTF_NUM=$(($INTF_NUM+$TMP_NUM))
  TMP_NUM=$(find $CVI_GADGET/functions/ -name "mass_storage*" | wc -l)
  EP_IN=$(($EP_IN+$TMP_NUM))
  EP_OUT=$(($EP_OUT+$TMP_NUM))
  INTF_NUM=$(($INTF_NUM+$TMP_NUM))
  TMP_NUM=$(find $CVI_GADGET/functions/ -name "cvg*" | wc -l)
  EP_IN=$(($EP_IN+$TMP_NUM))
  EP_OUT=$(($EP_OUT+$TMP_NUM))
  INTF_NUM=$(($INTF_NUM+$TMP_NUM))
  TMP_NUM=$(find $CVI_GADGET/functions/ -name "ncm*" | wc -l)
  EP_OUT=$(($EP_OUT+$TMP_NUM))
  TMP_NUM=$(($TMP_NUM * 2))
  EP_IN=$(($EP_IN+$TMP_NUM))
  INTF_NUM=$(($INTF_NUM+$TMP_NUM))
  TMP_NUM=$(find $CVI_GADGET/functions/ -name "uvc*" | wc -l)
  TMP_NUM=$(($TMP_NUM * 2))
  EP_IN=$(($EP_IN+$TMP_NUM))
  INTF_NUM=$(($INTF_NUM+$TMP_NUM))
  TMP_NUM=$(find $CVI_GADGET/functions/ -name "uac1*" | wc -l)
  TMP_NUM=$(($TMP_NUM * 2))
  EP_IN=$(($EP_IN+$TMP_NUM))
  EP_OUT=$(($EP_OUT+$TMP_NUM))
  INTF_NUM=$(($INTF_NUM+$TMP_NUM))
  TMP_NUM=$(find $CVI_GADGET/functions/ -name ffs.adb | wc -l)
  EP_IN=$(($EP_IN+$TMP_NUM))
  EP_OUT=$(($EP_OUT+$TMP_NUM))
  INTF_NUM=$(($INTF_NUM+$TMP_NUM))

  if [ "$CLASS" = "acm" ] ; then
    EP_IN=$(($EP_IN+2))
    EP_OUT=$(($EP_OUT+1))
  fi
  if [ "$CLASS" = "mass_storage" ] ; then
    EP_IN=$(($EP_IN+1))
    EP_OUT=$(($EP_OUT+1))
  fi
  if [ "$CLASS" = "cvg" ] ; then
    EP_IN=$(($EP_IN+1))
    EP_OUT=$(($EP_OUT+1))
  fi
  if [ "$CLASS" = "ncm" ] ; then
    EP_IN=$(($EP_IN+2))
    EP_OUT=$(($EP_OUT+1))
  fi
  if [ "$CLASS" = "uvc" ] ; then
    EP_IN=$(($EP_IN+2))
  fi
  if [ "$CLASS" = "uac1" ] ; then
    EP_IN=$(($EP_IN+1))
    EP_OUT=$(($EP_OUT+1))
  fi
  if [ "$CLASS" = "ffs.adb" ] ; then
    EP_IN=$(($EP_IN+1))
    EP_OUT=$(($EP_OUT+1))
  fi
  echo "$EP_IN in ep"
  echo "$EP_OUT out ep"
  if [ $EP_IN -gt $MAX_EP_NUM ]; then
    echo "reach maximum resource"
    exit 1
  fi
  if [ $EP_OUT -gt $MAX_EP_NUM ]; then
    echo "reach maximum resource"
    exit 1
  fi
}

probe() {
  if [ ! -d $CVI_DIR/usb_gadget ]; then
    mount none $CVI_DIR -t configfs 2>/dev/null || true
  fi
  if [ ! -d $CVI_GADGET ]; then
    mkdir -p $CVI_DIR/usb_gadget
    # Create gadget dev
    mkdir $CVI_GADGET
    # Set the VID and PID
    echo $VID >$CVI_GADGET/idVendor
    echo $PID >$CVI_GADGET/idProduct
    # Set the product information string
    mkdir $CVI_GADGET/strings/0x409
    echo $MANUFACTURER>$CVI_GADGET/strings/0x409/manufacturer
    echo $PRODUCT>$CVI_GADGET/strings/0x409/product
    echo $SERIAL>$CVI_GADGET/strings/0x409/serialnumber
    # Set the USB configuration
    mkdir $CVI_GADGET/configs/c.1
    mkdir $CVI_GADGET/configs/c.1/strings/0x409
    echo "config1">$CVI_GADGET/configs/c.1/strings/0x409/configuration
    # Set the MaxPower of USB descriptor
    echo 120 >$CVI_GADGET/configs/c.1/MaxPower
  fi
  # get current functions number
  calc_func
  # assign the class code for composite device
  if [ ! $FUNC_NUM -eq 0 ]; then
    echo 0xEF >$CVI_GADGET/bDeviceClass
    echo 0x02 >$CVI_GADGET/bDeviceSubClass
    echo 0x01 >$CVI_GADGET/bDeviceProtocol
  fi
  # resource check
  res_check
  # create the desired function
  if [ "$CLASS" = "ffs.adb" ] ; then
    # adb shall be the last function to probe. Override the pid/vid
    echo $VID >$CVI_GADGET/idVendor
    echo $PID >$CVI_GADGET/idProduct
    # choose pid for different function number
    if [ $INTF_NUM -eq 1 ]; then
      echo $ADB_PID_M1 >$CVI_GADGET/idProduct
    fi
    if [ $INTF_NUM -eq 2 ]; then
      echo $ADB_PID_M2 >$CVI_GADGET/idProduct
    fi
    mkdir $CVI_GADGET/functions/$CLASS
  else
    mkdir $CVI_GADGET/functions/$CLASS.usb$FUNC_NUM
  fi
  if [ "$CLASS" = "mass_storage" ] ; then
    echo $MSC_FILE >$CVI_GADGET/functions/$CLASS.usb$FUNC_NUM/lun.0/file
  fi
  if [ "$CLASS" = "ncm" ] ; then
    echo "$NCM_DEV_ADDR" >$CVI_FUNC/ncm.usb$FUNC_NUM/dev_addr
    echo "$NCM_HOST_ADDR" >$CVI_FUNC/ncm.usb$FUNC_NUM/host_addr
    #OS STRING
    echo 1 >$CVI_GADGET/os_desc/use
    echo 0xcd >$CVI_GADGET/os_desc/b_vendor_code
    echo MSFT100 >$CVI_GADGET/os_desc/qw_sign
    #COMPATIBLE ID: WINNCM makes Windows 10+ bind its inbox usbncm driver
    echo WINNCM >$CVI_FUNC/ncm.usb$FUNC_NUM/os_desc/interface.ncm/compatible_id
    #MAKE c.1 THE ONE ASSOCIATED WITH OS DESCRIPTORS
    ln -s $CVI_GADGET/configs/c.1 $CVI_GADGET/os_desc
  fi

}

start() {
  # link this function to the configuration
  calc_func
  if [ $FUNC_NUM -eq 0 ]; then
    echo "Functions Empty!"
    exit 1
  fi
  if [ -d $CVI_GADGET/functions/ffs.adb ]; then
    FUNC_NUM=$(($FUNC_NUM-1))
  fi
  for i in `seq 0 $(($FUNC_NUM-1))`;
  do
    find $CVI_GADGET/functions/ -name "*.usb$i" | xargs -I % ln -s % $CVI_GADGET/configs/c.1
  done
  if [ -d $CVI_GADGET/functions/ffs.adb ]; then
    ln -s $CVI_GADGET/functions/ffs.adb $CVI_GADGET/configs/c.1
    mkdir /dev/usb-ffs/adb -p
    mount -t functionfs adb /dev/usb-ffs/adb
    if [ -f $ADBD_PATH/adbd ]; then
	$ADBD_PATH/adbd &
    fi
  else
    # Start the gadget driver
    UDC=`ls /sys/class/udc/ | awk '{print $1}'`
    echo ${UDC} >$CVI_GADGET/UDC
  fi
}

stop() {
  if [ -d $CVI_GADGET/configs/c.1/ffs.adb ]; then
    pkill adbd
    rm $CVI_GADGET/configs/c.1/ffs.adb
  else
    echo "" >$CVI_GADGET/UDC
  fi
  find $CVI_GADGET/configs/ -name "*.usb*" | xargs rm -f
  rmdir $CVI_GADGET/configs/c.*/strings/0x409/
  # os_desc/c.1 is a symlink, so find -type d does not select it.  Unlink
  # the reference before removing its configuration; otherwise NCM
  # teardown leaves an unbound gadget that prevents the next mode starting.
  for config_link in "$CVI_GADGET"/os_desc/c.*; do
    [ -L "$config_link" ] || continue
    rm -- "$config_link"
  done
  rmdir $CVI_GADGET/configs/c.*/
  rmdir $CVI_GADGET/functions/*
  rmdir $CVI_GADGET/strings/0x409/
  rmdir $CVI_GADGET
}

case "$1" in
  start)
	start
	;;
  stop)
	stop
	;;
  probe)
	probe
	;;
  UDC)
  UDC=`ls /sys/class/udc/ | awk '{print $1}'`
  echo ${UDC} >$CVI_GADGET/UDC
	;;
  *)
	echo "Usage: $0 probe {acm|msc|cvg|uvc|uac1} {file (msc)}"
	echo "Usage: $0 start"
	echo "Usage: $0 stop"
	exit 1
esac
exit $?
