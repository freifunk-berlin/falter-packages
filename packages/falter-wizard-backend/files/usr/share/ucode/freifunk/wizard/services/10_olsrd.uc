'use strict';

const service = "/etc/init.d/olsrd";

function process(config) {
	system(service + " enable");
	system(service + " start");
	return true;
}

return {
	process,
};
