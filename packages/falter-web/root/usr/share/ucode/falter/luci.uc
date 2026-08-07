
'require luci.http';

// copied from luci.dispatcher
function build_url(...path) {
  let url = [ http.getenv('SCRIPT_NAME') ?? '' ];

  for (let p in path)
    if (match(p, /^[A-Za-z0-9_%.\/,;-]+$/))
      push(url, '/', p);

  if (length(url) == 1)
    push(url, '/');

  return join('', url);
}

return {
  action_back_to_falter() {
    http.redirect(build_url("falter"));
    return;
  }
};
