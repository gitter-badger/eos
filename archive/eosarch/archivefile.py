# ------------------------------------------------------------------------------
# File: archivefile.py
# Author: Elvin-Alin Sindrilaru <esindril@cern.ch>
# ------------------------------------------------------------------------------
#
# ******************************************************************************
# EOS - the CERN Disk Storage System
# Copyright (C) 2014 CERN/Switzerland
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
# ******************************************************************************
""" Class modelling an EOS archive file."""

import logging
import json
from XRootD import client
from XRootD.client.flags import MkDirFlags
from exceptions import CheckEntryException
from utils import exec_cmd, get_entry_info, set_dir_info


class ArchiveFile(object):
    """ Class modelling an EOS archive file.

    Attributes:
	file: File object pointing to local archive file.
	d2t: True if operation from disk to tape, otherwise False.
	header: Archive header dictionary.
    """

    def __init__(self, path, d2t):
	"""Initialize ArchiveFile object.

	Args:
	    path (str): Local path to archive file.
	    d2t (bool): True if transfer is to be disk to tape.

	Raises:
	    IOError: Failed to open local transfer file.
	"""
	self.logger = logging.getLogger("archivefile.transfer")
	self.d2t = d2t

	try:
	    self.file = open(path, 'r')
	except IOError as __:
	    self.logger.error("Failed to open file={0}".format(path))
	    raise

	line = self.file.readline()
	self.header = json.loads(line)
	self.fseek_dir = self.file.tell()  # save start position for dirs
	pos = self.fseek_dir

	while line:
	    line = self.file.readline()
	    entry = json.loads(line)

	    if entry[0] == 'f':
		self.fseek_file = pos  # save start position for files
		break

	    pos = self.file.tell()

	# Create two XRootD.FileSystem object for source and destination
	# which are to be reused throughout the transfer.
	self.fs_src = client.FileSystem(self.header['src'])
	self.fs_dst = client.FileSystem(self.header['dst'])
	self.logger.debug("fseek_dir={0}, fseek_file={1}".format(
		self.fseek_dir, self.fseek_file))

    def __del__(self):
	"""Destructor needs to close the file.
	"""
	try:
	    self.file.close()
	except ValueError as __:
	    self.logger.warning("File={0} already closed".format(self.file.name))

    def dirs(self):
	"""Generator to read directory entries from the archive file.

	Returns:
	    Return a directory entry from the archive file which looks like
	    this: ['d', "./rel/path/dir", "val1", ,"val2" ... ]
	"""
	self.file.seek(self.fseek_dir)
	line = self.file.readline()

	while line:
	    dentry = json.loads(line)

	    if dentry[0] == 'd':
		yield dentry
		line = self.file.readline()
	    else:
		break

    def files(self):
	"""Generator to read file entries from the archive file.

	Returns:
	    Return a file entry from the archive file which looks like this:
	    ['f', "./rel/path/file", "val1", ,"val2" ... ]
	"""
	self.file.seek(self.fseek_file)
	line = self.file.readline()

	while line:
	    fentry = json.loads(line)

	    if fentry[0] == 'f':
		yield fentry
		line = self.file.readline()
	    else:
		break

    def entries(self):
	""" Generator to read all entries from the archive file.

	Return:
	    A list representing a file or directory entry. See above for the
	    actual format.
	"""
	for dentry in self.dirs():
	    yield dentry

	for fentry in self.files():
	    yield fentry

    def get_fs(self, url):
	""" Get XRootD.FileSystem object matching the host in the url.

	Args:
	    url (string): XRootD endpoint URL.

	Returns:
	    FileSystem object to be used or None.
	"""
	if url.startswith(self.header['src']):
	    return self.fs_src
	elif url.startswith(self.header['dst']):
	    return self.fs_dst
	else:
	    return None

    def get_endpoints(self, rel_path):
	"""Get full source and destination URLs for the given relative path.

	For this use the information from the header. Take into account whether
	it is a disk to tape transfer or not. The src in header is always the
	disk and dst is the tape.

	Args:
	    rel_path (str): Entry relative path.

	Returns:
	    Return a tuple of string representing the source and the destination
	    of the transfer.
	"""
	if rel_path == "./":
	    rel_path = ""

	src = self.header['src'] + rel_path
	dst = self.header['dst'] + rel_path
	return (src, dst) if self.d2t else (dst, src)

    def del_entry(self, rel_path, is_dir, tape_delete):
	""" Delete file/dir.

	Args:
	    rel_path (str): Entry relative path as stored in the archive file.
	    is_dir (bool): True is entry is dir, otherwise False.
	    tape_delete(bool): If tape_delete is None the delete comes from a
		PUT or GET operations so we only use the value of self.d2t to
		decide which entry we will delete. If tape_delete is True we
		delete tape data, otherwise we purge (delete from disk only).

	Raises:
	    IOError: Deletion could not be performed.
	"""
	src, dst = self.get_endpoints(rel_path)

	if tape_delete is None:
	    surl = dst  # self.d2t is already used inside get_endpoints
	else:
	    surl = src if tape_delete else dst

	url = client.URL(surl)
	fs = self.get_fs(surl)
	self.logger.debug("Delete entry={0}".format(surl))

	if is_dir:
	    st_rm, __ = fs.rmdir(url.path + "?eos.ruid=0&eos.rgid=0")
	else:
	    st_rm, __ = fs.rm(url.path + "?eos.ruid=0&eos.rgid=0")

	if not st_rm.ok:
	    # Check if entry exists
	    st_stat, __ = fs.stat(url.path)

	    if st_stat.ok:
		err_msg = "Error removing entry={0}".format(surl)
		self.logger.error(err_msg)
		raise IOError()

	    self.logger.warning("Entry={0} already removed".format(surl))

    def make_mutable(self):
	""" Make the EOS sub-tree pointed by header['src'] mutable.

	Raises:
	    IOError when operation fails.
	"""
	url = client.URL(self.header['src'])

	for dentry in self.dirs():
	    dir_path = url.path + dentry[1]
	    fgetattr = ''.join([url.protocol, "://", url.hostid, "//proc/user/",
				"?mgm.cmd=attr&mgm.subcmd=get&mgm.attr.key=sys.acl",
				"&mgm.path=", dir_path])
	    (status, stdout, __) = exec_cmd(''.join(fgetattr))

	    if not status:
		warn_msg = "No xattr sys.acl found for dir={0}".format(dir_path)
		self.logger.warning(warn_msg)
	    else:
		# Remove the 'z:i' rule from the acl list
		acl_val = stdout[stdout.find('=') + 1:]
		pos = acl_val.find('z:i')

		if pos != -1:
		    if acl_val[pos - 1] == ',':
			acl_val = acl_val[:pos - 1] + acl_val[pos + 3:]
		    elif acl_val[pos + 3] == ',':
			acl_val = acl_val[pos + 3:]
		    else:
			acl_val = ''

		if acl_val:
		    # Set the new sys.acl xattr
		    fmutable = ''.join([url.protocol, "://", url.hostid, "//proc/user/?",
					"mgm.cmd=attr&mgm.subcmd=set&mgm.attr.key=sys.acl",
					"&mgm.attr.value=", acl_val, "&mgm.path=", dir_path])
		    (status, __, stderr) = exec_cmd(''.join(fmutable))

		    if not status:
			err_msg = "Error making dir={0} mutable, msg={1}".format(
			    dir_path, stderr)
			self.logger.error(err_msg)
			raise IOError(err_msg)
		else:
		    # sys.acl empty, remove it from the xattrs
		    frmattr = [url.protocol, "://", url.hostid, "//proc/user/?",
			       "mgm.cmd=attr&mgm.subcmd=rm&mgm.attr.key=sys.acl",
			       "&mgm.path=", dir_path]
		    (status, __, stderr) = exec_cmd(''.join(frmattr))

		    if not status:
			err_msg = ("Error removing xattr=sys.acl for dir={0}, msg={1}"
				   "").format(dir_path, stderr)
			self.logger.error(err_msg)
			raise IOError(err_msg)

    def check_root_dir(self):
	""" Do the necessary checks for the destination directory depending on
	the type of the transfer.

	Raises:
	     IOError: Root dir state inconsistent.
	"""
	root_str = self.header['dst' if self.d2t else 'src']
	fs = self.get_fs(root_str)
	url = client.URL(root_str)
	st, __ = fs.stat(url.path + "?eos.ruid=0&eos.rgid=0")

	if self.d2t and st.ok:
	    # For PUT destination dir must NOT exist
	    err_msg = "Root PUT dir={0} exists".format(root_str)
	    self.logger.error(err_msg)
	    raise IOError(err_msg)
	elif not self.d2t:
	    # For GET destination must exist and contain just the archive file
	    if not st.ok:
		err_msg = "Root GET dir={0} does NOT exist".format(root_str)
		self.logger.error(err_msg)
		raise IOError(err_msg)
	    else:
		ffindcount = [url.protocol, "://", url.hostid, "//proc/user/?"
			      "mgm.cmd=find&mgm.path=", url.path, "&mgm.option=Z"]
		(status, stdout, stderr) = exec_cmd(''.join(ffindcount))

		if status:
		    for entry in stdout.split():
			tag, num = entry.split('=')
			self.logger.debug("Tag={0}, val={1}".format(tag, num))

			if ((tag == 'nfiles' and num not in ['1', '2']) or
			    (tag == 'ndirectories' and num != '1')):
			    err_msg = ("Root GET dir={0} should contain at least "
				       "one file and at most two - clean up and "
				       "try again").format(root_str)
			    self.logger.error(err_msg)
			    raise IOError(err_msg)
		else:
		    err_msg = ("Error doing find count on GET destination={0}"
			       ", msg={1}").format(root_str, stderr)
		    self.logger.error(err_msg)
		    raise IOError(err_msg)

    def verify(self):
	""" Check the integrity of the archive either on disk or on tape.

	Returns:
	    (status, entry) - Status is True if archive is valid, otherwise
	    false. In case the archive has error return also the first corrupted
	    entry from the archive file, otherwise return an empty list.
	"""
	for entry in self.entries():
	    try:
		self._verify_entry(entry)
	    except CheckEntryException as __:
		self.logger.error("Archive verfication failed")
		return (False, entry)

	return (True, [])

    def _verify_entry(self, entry):
	""" Check that the entry (file/dir) has the proper meta data.

	Args:
	    entry (list): Entry from the arhive file containing all info about
	    this particular file/directory.

	Raises:
	    CheckEntryException if entry verification fails.
	"""
	is_dir, path = (entry[0] == 'd'), entry[1]
	__, dst = self.get_endpoints(path)
	url = client.URL(dst)

	if self.d2t:  # for PUT check that the entry exists and its size
	    fs = self.get_fs(dst)
	    st, stat_info = fs.stat(url.path)

	    if not st.ok:
		err_msg = "Entry={0} failed stat".format(dst)
		self.logger.error(err_msg)
		raise CheckEntryException("failed stat")

	    if not is_dir:  # for files check size match
		indx = self.header["file_meta"].index("size") + 2
		orig_size = int(entry[indx])

		if stat_info.size != orig_size:
		    err_msg = ("Verify file={0}, size={1}, expect_size={2}"
			       "").format(dst, orig_size, stat_info.size)
		    self.logger.error(err_msg)
		    raise CheckEntryException("failed file size match")
	else:  # for GET check all metadata
	    if is_dir:
		tags = ['uid', 'gid', 'attr']
	    else:
		tags = ['size', 'mtime', 'ctime', 'uid', 'gid', 'xstype', 'xs']

	    try:
		meta_info = get_entry_info(url, path, tags, is_dir)

		if not is_dir:
		    # TODO: review this - fix EOS to honour the mtime argument
		    # and don't remove it below and check also the file xs if
		    # possible
		    indx = self.header["file_meta"].index("mtime") + 2
		    del meta_info[indx]
		    del entry[indx]

	    except (AttributeError, IOError, KeyError) as __:
		self.logger.error("Failed getting metainfo entry={0}".format(dst))
		raise CheckEntryException("failed getting metainfo")

	    if not (meta_info == entry):
		err_msg = ("Verify failed for entry={0}, expect={1}, got={2}"
			   "").format(dst, entry, meta_info)
		self.logger.error(err_msg)
		raise CheckEntryException("failed metainfo match")

	self.logger.debug("Check={0}, status={1}".format(dst, True))

    def mkdir(self, dentry):
	""" Create directory and optionally for GET operations set the
	metadata information.

	Args:
	    dentry (list): Directory entry as read from the archive file.

	Raises:
	    IOError: Directory creation failed.
	"""
	__, surl = self.get_endpoints(dentry[1])
	fs = self.get_fs(surl)
	url = client.URL(surl)

	# Create directory
	st, __ = fs.mkdir(url.path + "?eos.ruid=0&eos.rgid=0",
			  MkDirFlags.MAKEPATH)

	if not st.ok:
	    err_msg = "Dir={0}, failed mkdir".format(surl)
	    self.logger.error(err_msg)
	    raise IOError(err_msg)

	# For GET operations set also the metadata
	if not self.d2t:
	    dict_dinfo = dict(zip(self.header['dir_meta'], dentry[2:]))

	    try:
		set_dir_info((surl, dict_dinfo))
	    except IOError as __:
		err_msg = "Dir={0}, failed setting metadata".format(surl)
		self.logger.error(err_msg)
		raise IOError(err_msg)