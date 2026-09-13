function bin16dec(bin) {
    var num=bin&0xFFFF;
    if (0x8000 & num)
        num = - (0x010000 - num);
    return num;
}
function bin8dec(bin) {
    var num=bin&0xFF;
    if (0x80 & num)
        num = - (0x0100 - num);
    return num;
}

function DecodebeeloggerPayload(data){
    var obj = {};
    var header = (data[data.length-2]<<8) + (data[data.length-1]);
//    obj.header = header;
//    obj.datalength = data.length;
    var i = 0;
    for(ID=0; ID<32; ID++){
        //console.log(data[i]);
        if(header&(1<<ID)) {
        switch(ID){
            case 0:
                var tempe=(data[i]<<8)|(data[i+1]);
                tempe=bin16dec(tempe);
                obj.TempOut=tempe/100;
                i+=2;
            break;
            case 1:
                var erh=(data[i]);
                obj.FeuchteOut=erh;
                i+=1;
            break;
            case 2:
                var vcc=(data[i]<<8)|(data[i+1]);
                obj.VBatt=vcc/1000;
                i+=2;
            break;
            case 3:
                var solar=(data[i]<<8)|(data[i+1]);
                obj.VSolar=solar/1000;
                i+=2;
            break;
            case 4:
                obj.Licht=(data[i]<<8)|(data[i+1]);
                i+=2;
            break;
            case 5:
               var weight=(data[i]<<8)|(data[i+1]);
               weight = bin16dec(weight);
               obj.Gewicht=weight/100;
               i+=2;
            break;
            case 6:
                var temp=(data[i]<<8)|(data[i+1]);
                temp=bin16dec(temp);
                obj.TempIn=temp/100;
                i+=2;
            break;
            case 7:
                var rh=(data[i]);
                obj.FeuchteIn=rh;
                i+=1;
            break;
            case 8:
               var weightb=(data[i]<<8)|(data[i+1]);
               weightb = bin16dec(weightb);
               obj.Gewicht2=weightb/100;
               i+=2;
            break;
            case 9:
                var tempb=(data[i]<<8)|(data[i+1]);
                tempb=bin16dec(tempb);
                obj.TempIn2=tempb/100;
                i+=2;
            break;
            case 10:
                var rhb=(data[i]);
                obj.FeuchteIn2=rhb;
                i+=1;
            break;
            case 11:
               var weightc=(data[i]<<8)|(data[i+1]);
               weightc = bin16dec(weightc);
               obj.Gewicht3=weightc/100;
               i+=2;
            break;
            case 12:
               var weightd=(data[i]<<8)|(data[i+1]);
               weightd = bin16dec(weightd);
               obj.Gewicht4=weightd/100;
               i+=2;
            break;
            case 13:
                var pres=(data[i]<<8)|(data[i+1]);
                pres=bin16dec(pres);
                obj.Aux1=pres/10;
                i+=2;
            break;          
            case 14:
                var svc=(data[i]);
                obj.Service=svc;
                i+=1;
            break;
            case 15:
                obj.Aux3=(data[i]<<24)|(data[i+1]<<16)|(data[i+2]<<8)|(data[i+3]);
                i+=4;
            break;
     
            default:
            break;
        }
        }
    }
    return obj;
}

function Decoder(bytes, port) {
  return DecodebeeloggerPayload(bytes);
}
