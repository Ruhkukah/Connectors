<xsl:stylesheet version="1.0" xmlns="http://www.w3.org/1999/xhtml" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:sbe="http://fixprotocol.io/2016/sbe">
    <xsl:output encoding="UTF-8" indent="yes" method="html" version="4.0"/>
    <xsl:template match="/sbe:messageSchema">
        <html>
            <head>
                <title>MOEX TWIME message schema</title>
                <style type="text/css">

                    body {
                        font: 100% Verdana, Geneva, Arial, Helvetica, sans-serif;
                        text-align: left;
                        line-height: 1.4rem;
                        color: #333;
                    }
     
                    h1 h2 h3 h4 {
                        padding: 1;
                        color: black;
                    }

                    h1 {
                        margin: 1rem 0 1rem 0;
                        font-size: 1.5rem;
                    }

                    h2 {
                        margin: 1rem 0 0.5rem 0;
                        font-size: 1.3rem;
                    }

                    h3 {
                        margin: 1.5rem 0 0.5rem 0;
                        font-size: 1.2rem;
                    }

                    h4 {
                        margin: 1rem 0 0.3rem 0;
                        font-size: 1.1rem;
                    }


                    table {
                        margin: 2rem 0 2rem 1rem;
                        border: 1px solid #ccc;
                        border-collapse: collapse;
                        empty-cells: show;
                        vertical-align: center;
                    }

                    table th, table td {
                        padding: 4px 12px 2px 12px;
                        text-align: left;
                        border: none;
                    }
                    
                    table tr.with-border, table td.with-border {
                        border: 1px solid #ccc;
                    }
                    
                    th {
                        font-weight: bold;
                        background: #f4f4f4;                    
                    }

                    div.gray {
                        color: #777;
                        font-weight: 100;
                    }

                </style>
            </head>
            <body style="padding: 1rem; padding-top: 1rem">
                <h1>MOEX TWIME message schema</h1>
                <h4>version = "<xsl:value-of select="/sbe:messageSchema/@version"/>",
                id = "<xsl:value-of select="/sbe:messageSchema/@id"/>"</h4>
                <hr/>
                <h2>
                    <a href="#Messages">Messages</a>
                </h2>
                <ul style="list-style-type:circle">
                    <xsl:for-each select="/sbe:messageSchema/sbe:message">
                        <li>
                            <a href="#{@name}">
                                <xsl:value-of select="@name"/>
                            </a>
                        </li>
                    </xsl:for-each>
                </ul>
                <h2>
                    <a href="#Enumerations">Enumerations</a>
                </h2>
                <ul style="list-style-type:circle">
                    <xsl:for-each select="/sbe:messageSchema/types/enum">
                        <li>
                            <a href="#{@name}">
                                <xsl:value-of select="@name"/>
                            </a>
                        </li>
                    </xsl:for-each>
                </ul>
                <h2>
                    <a href="#Sets">Sets</a>
                </h2>
                <ul style="list-style-type:circle">
                    <xsl:for-each select="/sbe:messageSchema/types/set">
                        <li>
                            <a href="#{@name}">
                                <xsl:value-of select="@name"/>
                            </a>
                        </li>
                    </xsl:for-each>
                </ul>
                <h2>
                    <a href="#Composites">Composites</a>
                </h2>
                <ul style="list-style-type:circle">
                    <xsl:for-each select="/sbe:messageSchema/types/composite">
                        <li>
                            <a href="#{@name}">
                                <xsl:value-of select="@name"/>
                            </a>
                        </li>
                    </xsl:for-each>
                </ul>
				
                <h2>
                    <a href="#Types">Types</a>
                </h2>
                <hr/>
				
                <p style="height: 1.5cm"/>
                
				<h2>
                    <a name="Messages">Messages</a>
                </h2>
                <hr/>
                <xsl:for-each select="/sbe:messageSchema/sbe:message">
                    <table width="55%">
                        <tr>
                            <td class="with-border" colspan="4">
                                <h3>
                                    <a name="{@name}">
                                    <xsl:value-of select="@name"/>
                                    </a>
                                </h3>
                            </td>
                        </tr>
                        <tr>
                            <th>Name</th>
                            <th>Id</th>
                            <th/>
                        </tr>
                        <tr>
                            <td>
                                <xsl:value-of select="@name"/>
                            </td>
                            <td>
                                <xsl:value-of select="@id"/>
                            </td>
                            <td/>
                        </tr>
                        <tr>
                            <td colspan="3"/>
                        </tr>
                        <tr>
                            <td class="with-border" colspan="4">
                                <h4>Fields</h4>
                            </td>
                        </tr>
                        <tr>
                            <th width="45%">Name</th>
                            <th width="10%">Id</th>
                            <th width="45%">Type</th>
                        </tr>
                        <xsl:for-each select="(.)/field">
                            <tr>
                                <td>
                                    <xsl:value-of select="@name"/>
                                </td>
                                <td>
                                    <xsl:value-of select="@id"/>
                                </td>
                                <td>
									<a href="#{@type}">
										<xsl:value-of select="@type"/>
									</a>
                                </td>
                            </tr>
                        </xsl:for-each>
                    </table>
                </xsl:for-each>
                <hr/>
                <p style="height: 1.5cm"/>
                <h2>
                    <a name="Enumerations">Enumerations</a>
                </h2>
                <!--xsl:for-each select="/sbe:messageSchema/sbe:message">
                        <a href=><td colspan='4'><h4><xsl:value-of select="@name"/></h4></td></tr>
                    </xsl:for-each-->
                <hr/>
                <xsl:for-each select="/sbe:messageSchema/types/enum">
                    <table width="30%">
                        <tr>
                            <td class="with-border" colspan="2">
                                <h3>
                                    <a name="{@name}">
                                    <xsl:value-of select="@name"/>
                                    </a>
                                </h3>
                            </td>
                        </tr>
                        <tr>
                            <th>Name</th>
                            <th style="text-align: right;">Encoding type</th>
                        </tr>
                        <tr>
                            <td>
                                <xsl:value-of select="@name"/>
                            </td>
                            <td style="text-align: right;">
                                <xsl:value-of select="@encodingType"/>
                            </td>
                        </tr>
                        <tr>
                            <td colspan="2"/>
                        </tr>
                        <tr>
                            <td class="with-border" colspan="2">
                                <h4>Values</h4>
                            </td>
                        </tr>
                        <tr>
                            <th width="60%">Name</th>
                            <th style="text-align: right" width="40%">Value</th>
                        </tr>
                        <xsl:for-each select="(.)/validValue">
                            <tr>
                                <td>
                                    <xsl:value-of select="@name"/>
                                </td>
                                <td style="text-align: right">
                                    <xsl:value-of select="text()"/>
                                </td>
                            </tr>
                        </xsl:for-each>
                    </table>
                </xsl:for-each>
                <hr/>
                <p style="height: 1.5cm"/>
                <h2>
                    <a name="Sets">Sets</a>
                </h2>
                <hr/>
                <xsl:for-each select="/sbe:messageSchema/types/set">
                    <table width="30%">
                        <tr>
                            <td class="with-border" colspan="2">
                                <h3>
                                    <a name="{@name}">
                                    <xsl:value-of select="@name"/>
                                    </a>
                                </h3>
                            </td>
                        </tr>
                        <tr>
                            <th>Name</th>
                            <th style="text-align: right;">Encoding type</th>
                        </tr>
                        <tr>
                            <td>
                                <xsl:value-of select="@name"/>
                            </td>
                            <td style="text-align: right;">
                                <xsl:value-of select="@encodingType"/>
                            </td>
                        </tr>
                        <tr>
                            <td colspan="2"/>
                        </tr>
                        <tr>
                            <td class="with-border" colspan="2">
                                <h4>Values</h4>
                            </td>
                        </tr>
                        <tr>
                            <th width="60%">Name</th>
                            <th style="text-align: right" width="40%">Value</th>
                        </tr>
                        <xsl:for-each select="(.)/choice">
                            <tr>
                                <td>
                                    <xsl:value-of select="@name"/>
                                </td>
                                <td style="text-align: right">
                                    <xsl:value-of select="text()"/>
                                </td>
                            </tr>
                        </xsl:for-each>
                    </table>
                </xsl:for-each>
                <hr/>

                <p style="height: 1.5cm"/>
                <h2>
                    <a name="Composites">Composites</a>
                </h2>
                <hr/>
                <xsl:for-each select="/sbe:messageSchema/types/composite">
                    <table width="80%">
						<tr>
                            <td class="with-border" colspan="6">
                                <h3>
                                    <a name="{@name}">
                                    <xsl:value-of select="@name"/>
                                    </a>
                                </h3>
                            </td>
                        </tr>
                        <tr>
                            <th>Name</th>
                            <th style="text-align: left;" colspan="5" width="84%">Description</th>
                        </tr>
                        <tr>
                            <td>
                                <xsl:value-of select="@name"/>
                            </td>
                            <td colspan="5" width="84%">
                                <xsl:value-of select="@description"/>
                            </td>
                        </tr>
                        <tr>
                            <td colspan="6"/>
                        </tr>
						<tr>
							<th width="16%">Name</th>
							<th style="text-align: right;" width="12%">Primitive type</th>
							<th style="text-align: right;" width="18%">Min value</th>
							<th style="text-align: right;" width="18%">Max value</th>
							<th style="text-align: right;" width="18%">Null value</th>
							<th style="text-align: right;" width="18%">Presence</th>
						</tr>
						<xsl:for-each select="(.)/type">
							<tr class="with-border">
								<td>
									<xsl:value-of select="@name"/>
								</td>
								<td style="text-align: right">
									<xsl:value-of select="@primitiveType"/>
								</td>
								<td style="text-align: right;">
									<xsl:call-template name="t_composite">
										<xsl:with-param name="node" select="@minValue"/>
									</xsl:call-template>
								</td>
								<td style="text-align: right;">
									<xsl:call-template name="t_composite">
										<xsl:with-param name="node" select="@maxValue"/>
									</xsl:call-template>
								</td>
								<td style="text-align: right;">
									<xsl:call-template name="t_composite">
										<xsl:with-param name="node" select="@nullValue"/>
									</xsl:call-template>
								</td>
								<td style="text-align: right;">
									<xsl:call-template name="t_composite">
										<xsl:with-param name="node" select="@presence"/>
										<xsl:with-param name="text" select="text()"/>
									</xsl:call-template>
								</td>
							</tr>
						</xsl:for-each>
					</table>
                </xsl:for-each>
                <hr/>
				
				
                <p style="height: 1.5cm"/>
                <h2>
                    <a name="Types">Types</a>
                </h2>
                <hr/>
                <table width="80%">
                    <tr>
                        <th width="16%">Name</th>
                        <th style="text-align: right;" width="12%">Primitive type</th>
                        <th style="text-align: right;" width="18%">Min value</th>
                        <th style="text-align: right;" width="18%">Max value</th>
                        <th style="text-align: right;" width="18%">Null value</th>
                        <th style="text-align: right;" width="18%">Presence</th>
                    </tr>
                    <xsl:for-each select="/sbe:messageSchema/types/type">
                        <tr class="with-border">
                            <td>
								<a name="{@name}">
                                    <xsl:value-of select="@name"/>
                                </a>
                            </td>
                            <td style="text-align: right">
                                <xsl:value-of select="@primitiveType"/>
                            </td>
                            <td style="text-align: right;">
                                <xsl:call-template name="t_type">
                                    <xsl:with-param name="node" select="@minValue"/>
                                </xsl:call-template>
                            </td>
                            <td style="text-align: right;">
                                <xsl:call-template name="t_type">
                                    <xsl:with-param name="node" select="@maxValue"/>
                                </xsl:call-template>
                            </td>
                            <td style="text-align: right;">
                                <xsl:call-template name="t_type">
                                    <xsl:with-param name="node" select="@nullValue"/>
                                </xsl:call-template>
                            </td>
                            <td style="text-align: right;">
                                <xsl:call-template name="t_type">
                                    <xsl:with-param name="node" select="@presence"/>
                                </xsl:call-template>
                            </td>
                        </tr>
                    </xsl:for-each>
                </table>
                <hr/>
                <div class="gray">(End of document)</div>
                <p style="height: 700px"/>
            </body>
        </html>
    </xsl:template>
    <xsl:template name="t_type">
        <xsl:param name="node"/>
        <xsl:if test="$node">
            <xsl:value-of select="$node"/>
        </xsl:if>
        <xsl:if test="not($node)">
            <xsl:text>N/A</xsl:text>
        </xsl:if>
    </xsl:template>
	<xsl:template name="t_composite">
        <xsl:param name="node"/>
		<xsl:param name="text"/>
        <xsl:if test="$node">
            <xsl:value-of select="$node"/>
			<xsl:if test="$node='constant'">
				<xsl:text> = </xsl:text>
				<xsl:value-of select="$text"/>
			</xsl:if>
        </xsl:if>
        <xsl:if test="not($node)">
            <xsl:text>SBE default</xsl:text>
        </xsl:if>
    </xsl:template>	
</xsl:stylesheet>
