'use strict';

let ffutils = require("freifunk.utils");
let fs = require("fs");

const profile_dir = "/etc/freifunk/profiles/";
const community = global.cursor.get("freifunk", "community", "name") || "berlin";

// deepMerge taken from
//       https://ucode-lang.org/ucode/tutorial-03-objects.html#merging
function deepMerge(target, ...sources) {
    for (let source in sources) {
        if (type(source) != "object")
            continue;

        for (let key in keys(source)) {
            if (type(source[key]) == "object" && type(target[key]) == "object")
                target[key] = deepMerge({ ...target[key] }, source[key]);
            else
                target[key] = source[key];
        }
    }

    return target;
}

function get_defaults(config, section) {
	let freifunk = json(fs.readfile(profile_dir + "freifunk.json"));

	let profile = json(fs.readfile(profile_dir + community + ".json"));

	let combined = deepMerge({}, 
			freifunk[config][section],
			profile[config]?.[section] ?? {});
	return combined
}

return {
	community,
	get_defaults,
};
