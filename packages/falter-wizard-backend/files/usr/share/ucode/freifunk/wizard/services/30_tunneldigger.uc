'use strict';

const service = "/etc/init.d/tunneldigger";

function process(config) {
	system(service + " start");
	return true;
}

return {
	process,
};
