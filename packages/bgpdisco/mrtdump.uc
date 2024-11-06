import { pack, unpack } from 'struct';
import { open } from 'fs';
import { DBG, INFO, WARN, ERR } from 'bgpdisco.logger';

function get_routes(filename) {
  let _routes = [];
  function process_prefix(fd, address_size) {
    // Seqno, Prefix Length
    const RIB_ENTRY_SEQPFXL_FMT = '>IB';
    const RIB_ENTRY_SEQPFXL_LEN=5;
    let seqpfxl = fd.read(RIB_ENTRY_SEQPFXL_LEN);
    let _seqpfxl = unpack(RIB_ENTRY_SEQPFXL_FMT, seqpfxl);

    let entry_seqno = _seqpfxl[0];
    let entry_prefix_length = _seqpfxl[1];

    const RIB_ENTRY_PREFIX_FMT = '>' + address_size + 'B';
    let pfx = fd.read(address_size);
    let entry_prefix = arrtoip(unpack(RIB_ENTRY_PREFIX_FMT, pfx));
    // print("- Seq ", entry_seqno, ":", entry_prefix, "/", entry_prefix_length, "\n");

    let entrycnt= fd.read(2);
    let entry_count = unpack('>H', entrycnt)[0];
    // print("    Entry Count: ", entry_count, "\n");

    const RIB_ENTRY_VIA_HDR_FMT=">HIH";
    for (let i=0; i<entry_count; i++) {
      let rt = fd.read(8);
      let _rt = unpack(RIB_ENTRY_VIA_HDR_FMT, rt);

      let route_peer_idx = _rt[0];
      let route_timestamp = _rt[1];
      let route_attribute_len = _rt[2];
      // print("NextHop unpacked: ", _rt, "\n");

      let __route_info = {
        prefix: entry_prefix + '/' + entry_prefix_length,
        next_hop: route_peer_idx,
        timestamp: route_timestamp,
        attributes: {}
      };

      while (route_attribute_len > 0) {
        // print('--Reading atribute', '\n');
        let ft = unpack('>cB', fd.read(2)); // [flags, type]
        let attr_flags = ft[0];
        let attr_type = ft[1];

        let _field_attr_len = 1;
        let _field_attr_unpack_str = '>B';

        // set len to two bytes if extended attribute length flag (4) is set
        if (attr_flags & 0x01 << 4) {
          print('  Extended attribute length!', '\n');
          _field_attr_len = 2;
          _field_attr_unpack_str = '>H';
        }

        let attr_len = unpack(_field_attr_unpack_str, fd.read(_field_attr_len))[0];
        // print("--Processing RtAttr type:", attr_type, ", length: ", attr_len, '\n');

	let attr_data;
        if (attr_len > 0) {
          let _attributes = fd.read(attr_len);
          let attr_data_unpack_str = '>*';
          attr_data = unpack(attr_data_unpack_str, _attributes)[0];
        }

        route_attribute_len -= 2 + _field_attr_len + attr_len;

        __route_info.attributes[attr_type] = attr_data;
      }

      push(_routes, __route_info);
    }
    return false;
  }


  function process_record(fd, timestamp, type, subtype, length) {
    // print("Process Record. Header: Timestamp: ", timestamp, "; Type: ", type, "; Subtype: ", subtype, "; Length: ", length, "\n");

    const MRT_ENTRY_TYPE_TABLE_DUMP_V2 = 13;

    const MRT_ENTRY_SUBTYPE_PEER_INDEX_TABLE = 1;

    const MRT_ENTRY_SUBTYPE_RIB_IPV4 = 2;
    const MRT_ENTRY_SUBTYPE_RIB_IPV4_MC = 3;
    const MRT_ENTRY_SUBTYPE_RIB_IPV6 = 4;
    const MRT_ENTRY_SUBTYPE_RIB_IPV6_MC = 5;
    const MRT_ENTRY_SUBTYPE_RIB_IPV4_ADD = 8;
    const MRT_ENTRY_SUBTYPE_RIB_IPV4_MC_ADD = 9;
    const MRT_ENTRY_SUBTYPE_RIB_IPV6_ADD = 10;
    const MRT_ENTRY_SUBTYPE_RIB_IPV6_MC_ADD = 11;

    if (!( type == MRT_ENTRY_TYPE_TABLE_DUMP_V2 && ( subtype == MRT_ENTRY_SUBTYPE_RIB_IPV4 || subtype == MRT_ENTRY_SUBTYPE_RIB_IPV6))) {
      //print("Skipping unsuported record", "\n");
      fd.read(length);
      return;
    }

   let address_size = 16; // IPv6
   if (subtype == 2 || subtype == 3 || subtype == 8 || subtype == 9) {
     address_size = 4; // IPv4
   }


    while (process_prefix(fd, address_size)) {
      //print("Processing Prefix..");
    }
  }


  function read_header(fd) {
    const MRT_HEADER_FMT = '>IHHI';
    const MRT_HEADER_LEN = 12;
    let hdr = fd.read(MRT_HEADER_LEN);
    if (!hdr) {
      // print ("EOF");
      return false;
    }
    let _hdr = unpack(MRT_HEADER_FMT, hdr);
    process_record(fd, _hdr[0], _hdr[1], _hdr[2], _hdr[3]);
    return true;
  }

  DBG('Reading MRT file %s', filename);

  let fd = open(filename, 'r');
  while (read_header(fd));

  return _routes;
}
export { get_routes };
