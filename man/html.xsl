<?xml version='1.0'?> <!--*-nxml-*-->

<!--
Copyright © 2022 Endless OS Foundation LLC

SPDX-License-Identifier: LGPL-2.0+

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library. If not, see <https://www.gnu.org/licenses/>.
-->

<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">

<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/html/docbook.xsl"/>

<!-- translate man page references to links to html pages -->
<xsl:template match="citerefentry">
  <a>
    <xsl:attribute name="href">
      <xsl:value-of select="refentrytitle"/>
      <xsl:text>.html</xsl:text>
    </xsl:attribute>
    <xsl:call-template name="inline.charseq"/>
  </a>
</xsl:template>

</xsl:stylesheet>
