import { chdir, glob, readfile } from "fs";
chdir("/sys/class/hwmon/");
let tempgauge = gauge("temp");
for (let input in glob("**/*temp*_input")){
    let temperature = int(trim(readfile(input)));
    let name = trim(readfile(replace(input, /\/.*/, "/name")));
    let label = trim(readfile(replace(input, "_input", "_label")));
    if (label) {
        label = "_" + label;
    } else {
        label = "";
    }
    let namelabel = name + label;
    tempgauge({id: namelabel}, temperature);
}
